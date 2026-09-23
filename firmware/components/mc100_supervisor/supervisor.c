#include "mc100_supervisor.h"

#include <stdlib.h>
#include <string.h>

struct mc100_supervisor {
    mc100_supervisor_deps_t deps;
    mc100_state_t *state;
    mc100_audio_t *audio;
    mc100_writer_t *writer;
    mc100_assembler_t assembler;
    uint8_t pcm_buffer[MC100_FRAME_SAMPLES * sizeof(int16_t)];
    mc100_generation_t armed_generation;
    mc100_generation_t recording_generation;
    uint32_t silent_frames;
    bool silence_pending;
    bool capture_stopping;
    bool writer_started;
    bool booted;
    bool dispatching_fault;
    bool hold_retry_pending;
    mc100_action_t hold_retry_action;
    bool release_retry_pending;
    mc100_action_t release_retry_action;
    mc100_event_t event_queue[MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY];
    size_t event_head;
    size_t event_tail;
    size_t event_count;
    bool critical_pending;
    mc100_event_t critical_event;
    uint64_t time_watermark;
};

static uint64_t now(mc100_supervisor_t *s)
{
    uint64_t sampled = s->deps.now_ms(s->deps.clock_ctx);
    if (sampled > s->time_watermark)
        s->time_watermark = sampled;
    return s->time_watermark;
}

static uint64_t monotonic_event_time(mc100_supervisor_t *s, uint64_t proposed)
{
    if (proposed > s->time_watermark) s->time_watermark = proposed;
    return s->time_watermark;
}

static void event_lock(mc100_supervisor_t *s)
{
    if (s->deps.event_lock != NULL && s->deps.event_unlock != NULL)
        s->deps.event_lock(s->deps.event_lock_ctx);
}

static void event_unlock(mc100_supervisor_t *s)
{
    if (s->deps.event_lock != NULL && s->deps.event_unlock != NULL)
        s->deps.event_unlock(s->deps.event_lock_ctx);
}

static bool external_event_id(mc100_event_id_t id)
{
    return id == MC100_EV_LOW || id == MC100_EV_CRITICAL ||
           id == MC100_EV_FAULT || id == MC100_EV_TICK ||
           id == MC100_EV_RECOVERED_POWER;
}

static bool nondroppable_event(const mc100_event_t *event)
{
    /* ADC_INVALID and card-removal indications arrive as FAULT/CRITICAL;
     * preserving every FAULT also prevents a diagnostic safety intent from
     * disappearing behind a burst of harmless TICK notifications. */
    return event->id == MC100_EV_CRITICAL || event->id == MC100_EV_FAULT;
}

static mc100_fault_reason_t audio_reason(mc100_result_t r)
{
    return r == MC100_FULL ? MC100_FAULT_QUEUE_OVERFLOW :
           r == MC100_IO ? MC100_FAULT_MIC_IO : MC100_FAULT_INTERNAL_PROTOCOL;
}
static mc100_fault_reason_t storage_reason(mc100_result_t r)
{
    return r == MC100_FULL ? MC100_FAULT_STORAGE_FULL :
           r == MC100_TIMEOUT ? MC100_FAULT_STORAGE_TIMEOUT :
           r == MC100_IO ? MC100_FAULT_STORAGE_IO :
           MC100_FAULT_INTERNAL_PROTOCOL;
}

static mc100_result_t pump(mc100_supervisor_t *, const mc100_event_t *);
static mc100_result_t action(mc100_supervisor_t *, const mc100_action_t *,
                             uint64_t);

static mc100_result_t raise_fault(mc100_supervisor_t *s,
                                  mc100_fault_reason_t reason)
{
    if (!s || s->dispatching_fault) return MC100_INVALID;
    s->dispatching_fault = true;
    mc100_event_t e = {.id = MC100_EV_FAULT, .now_ms = now(s),
                       .detail = (uint32_t)reason, .global = true};
    mc100_result_t r = pump(s, &e);
    s->dispatching_fault = false;
    return r;
}

static mc100_result_t on_frame(void *ctx, const mc100_frame_t *frame)
{
    mc100_supervisor_t *s = ctx;
    bool speech = s->deps.vad.decide != NULL &&
                  s->deps.vad.decide(s->deps.vad.ctx, frame);
    mc100_result_t r = mc100_audio_push(s->audio, frame, speech);
    if (r != MC100_OK) return r;
    if (s->recording_generation && !s->capture_stopping) {
        if (speech) s->silent_frames = 0;
        else if (s->silent_frames < MC100_ARM_SILENCE_FRAMES &&
                 ++s->silent_frames == MC100_ARM_SILENCE_FRAMES)
            s->silence_pending = true;
    }
    return MC100_OK;
}

static mc100_result_t boot_failure(mc100_supervisor_t *s, mc100_result_t cause)
{
    mc100_writer_abandon(s->writer, MC100_FAULT_RECOVERY_REQUIRED);
    mc100_result_t r = mc100_writer_release_handles(s->writer);
    (void)raise_fault(s, MC100_FAULT_RECOVERY_REQUIRED);
    return r != MC100_OK ? r : cause;
}

static mc100_result_t writer_start(mc100_supervisor_t *s,
                                   mc100_generation_t g, uint64_t first)
{
    mc100_writer_status_t st;
    mc100_result_t r = mc100_writer_status(s->writer, &st);
    if (r != MC100_OK) return r;
    if (st.active) return st.generation == g ? MC100_OK : MC100_NOT_READY;
    if (!st.prepared_slots) {
        r = mc100_writer_prepare(s->writer);
        if (r != MC100_OK) return r;
    }
    r = mc100_writer_begin(s->writer, g, first);
    if (r == MC100_OK) s->writer_started = true;
    return r;
}

static mc100_result_t release_audio(mc100_supervisor_t *s,
                                    mc100_generation_t g)
{
    if (!g) return MC100_OK;
    mc100_result_t r = mc100_audio_release(s->audio, g);
    return r == MC100_INVALID ? MC100_OK : r;
}

/* The writer is the sole authority for final names. Consume its bounded
 * publication queue at the supervisor boundary; no caller may reconstruct a
 * name from boot/generation/segment fields. */
static mc100_result_t publish_pending(mc100_supervisor_t *s)
{
    for (;;) {
        mc100_writer_publication_t publication = {0};
        mc100_result_t r =
            mc100_writer_publication_pop(s->writer, &publication);
        if (r == MC100_NOT_READY)
            return MC100_OK;
        if (r != MC100_OK)
            return r;
        if (s->deps.upload.notify_closed != NULL)
            s->deps.upload.notify_closed(s->deps.upload.ctx,
                                         publication.name);
    }
}

static mc100_result_t drain(mc100_supervisor_t *s, mc100_generation_t g)
{
    for (;;) {
        mc100_packet_t p;
        mc100_result_t r = mc100_audio_peek(s->audio, &p);
        if (r == MC100_NOT_READY) return MC100_OK;
        if (r != MC100_OK) return r;
        /* FIFO order puts an already-closing session before its successor.
         * Reaching another generation therefore completes this drain; leave
         * that packet queued for its owning session. */
        if (p.generation != g) return MC100_OK;
        r = mc100_audio_pop(s->audio, &p);
        if (r != MC100_OK) return r;
        if (!s->writer_started) {
            r = writer_start(s, g, p.frame.seq);
            if (r != MC100_OK) return r;
        }
        r = mc100_writer_append(s->writer, &p);
        /* A segment may be finalized by this append. Publish it immediately
         * so the bounded queue cannot fill while RECORD continues. */
        mc100_result_t published = publish_pending(s);
        if (r != MC100_OK) return r;
        if (published != MC100_OK) return published;
    }
}

static mc100_result_t action(mc100_supervisor_t *s,
                             const mc100_action_t *a, uint64_t at)
{
    mc100_result_t r;
    switch (a->id) {
    case MC100_ACT_ARM: {
        if (s->deps.audio_control != NULL) {
            r = s->deps.audio_control(s->deps.audio_control_ctx,
                                      MC100_SUPERVISOR_AUDIO_START,
                                      a->generation);
            if (r != MC100_OK) {
                (void)raise_fault(s, MC100_FAULT_MIC_IO);
                return r;
            }
        }
        r = mc100_audio_arm(s->audio, a->generation,
                            a->seq_valid ? a->seq : 0);
        if (r != MC100_OK) {
            (void)raise_fault(s, audio_reason(r));
            return r;
        }
        s->armed_generation = a->generation;
        s->capture_stopping = false;
        mc100_event_t e = {.id = MC100_EV_ARMED, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_OPEN: {
        mc100_snapshot_t snap = {0};
        bool snapshot_acquired = false;
        r = mc100_audio_snapshot(s->audio, a->generation, &snap);
        if (r == MC100_OK) {
            snapshot_acquired = true;
            if (snap.count)
                r = writer_start(s, a->generation, snap.first_seq);
        }
        mc100_packet_t p = {.generation = a->generation};
        if (r == MC100_OK) {
            for (uint16_t i = 0; i < snap.count; ++i) {
                r = mc100_audio_snapshot_frame(s->audio, &snap, i, &p.frame);
                if (r != MC100_OK) break;
                r = mc100_writer_append(s->writer, &p);
                if (r != MC100_OK) break;
            }
        }
        if (snapshot_acquired) {
            mc100_result_t released = mc100_audio_snapshot_release(
                s->audio, a->generation);
            if (r == MC100_OK) r = released;
        }
        if (r != MC100_OK) {
            (void)raise_fault(s, storage_reason(r));
            return r;
        }
        r = publish_pending(s);
        if (r != MC100_OK) {
            (void)raise_fault(s, storage_reason(r));
            return r;
        }
        s->recording_generation = a->generation;
        s->silent_frames = 0;
        s->silence_pending = false;
        mc100_event_t e = {.id = MC100_EV_OPENED, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_STOP_CAPTURE: {
        uint64_t cutoff = 0;
        if (s->deps.audio_control != NULL) {
            r = s->deps.audio_control(s->deps.audio_control_ctx,
                                      MC100_SUPERVISOR_AUDIO_STOP,
                                      a->generation);
            if (r != MC100_OK) {
                (void)raise_fault(s, MC100_FAULT_MIC_IO);
                return r;
            }
        }
        r = mc100_audio_stop(s->audio, a->generation, &cutoff);
        if (r != MC100_OK) {
            /* Do not acknowledge a failed stop.  The state machine must take
             * the no-write HOLD path rather than closing with a fabricated
             * cutoff or leaving a stop_wait transaction unowned. */
            (void)raise_fault(s, MC100_FAULT_INTERNAL_PROTOCOL);
            return r;
        }
        mc100_audio_status_t st = {0};
        mc100_result_t observed = mc100_audio_status(s->audio, a->generation,
                                                      &st);
        if (observed != MC100_OK) {
            (void)raise_fault(s, MC100_FAULT_INTERNAL_PROTOCOL);
            return observed;
        }
        s->capture_stopping = true;
        mc100_event_t e = {.id = MC100_EV_CAPTURE_STOPPED,
                           .generation = a->generation, .seq = cutoff,
                           .now_ms = at,
                           .seq_valid = observed == MC100_OK && st.cutoff_valid};
        mc100_result_t t = pump(s, &e);
        return r != MC100_OK ? r : t;
    }
    case MC100_ACT_CLOSE_THROUGH: {
        mc100_result_t drained = drain(s, a->generation);
        mc100_writer_status_t st = {0};
        mc100_result_t observed = mc100_writer_status(s->writer, &st);
        bool full_incident = observed == MC100_OK && !st.active &&
                             st.latched_reason ==
                                 MC100_INCIDENT_STORAGE_FULL;
        r = drained;
        if ((drained == MC100_OK || drained == MC100_FULL) && full_incident) {
            /* append() may discover exhausted admission while CLOSE drains
             * already accepted audio.  In that case writer has itself
             * durably finalized the safe prefix as .partial.wav; acknowledge
             * that close instead of attempting it a second time. */
            r = MC100_OK;
            if (mc100_state_get(s->state) != MC100_FAULT)
                r = raise_fault(s, MC100_FAULT_STORAGE_FULL);
        } else if (r == MC100_OK) {
            r = observed;
        }
        if (r == MC100_OK && a->seq_valid) {
            if (!st.active) {
                if (!full_incident) r = MC100_NOT_READY;
            } else if (!st.has_frames || st.generation != a->generation ||
                       st.last_seq != a->seq)
                r = MC100_CORRUPT;
            else {
                r = mc100_writer_close_through(s->writer, a->generation,
                                               a->seq, a->detail);
            }
        } else if (r == MC100_OK && s->writer_started) {
            mc100_writer_abandon(s->writer, a->detail);
            r = mc100_writer_release_handles(s->writer);
        }
        if (r != MC100_OK) {
            /* Earlier rotations may already be durable even when this close
             * fails. Do not lose their publication records while routing the
             * close failure to FAULT. Any error from the close transaction,
             * including FULL, is no longer a healthy admission failure: the
             * terminal metadata may be incomplete, so force the no-write HOLD
             * path instead of attempting another safe-prefix close. */
            (void)publish_pending(s);
            (void)raise_fault(s, MC100_FAULT_STORAGE_IO);
            return r;
        }
        r = publish_pending(s);
        if (r != MC100_OK) {
            (void)raise_fault(s, storage_reason(r));
            return r;
        }
        s->writer_started = false;
        mc100_event_t e = {.id = MC100_EV_CLOSED, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_RELEASE:
        r = release_audio(s, a->generation);
        /* State has already emitted CLOSED/HELD, but the owner must not drop
         * its local ownership token until the underlying audio release has
         * actually succeeded.  A failed cleanup is retriable/diagnosable;
         * clearing these fields would falsely advertise quiescence and could
         * let a later generation reuse buffers still referenced by Audio. */
        if (r != MC100_OK) {
            /* State has already emitted CLOSED and may have selected LISTEN;
             * a failed owner release must immediately revoke that apparent
             * readiness and enter the no-write fault/hold path. */
            (void)raise_fault(s, MC100_FAULT_INTERNAL_PROTOCOL);
            return r;
        }
        if (a->generation == s->recording_generation)
            s->recording_generation = 0;
        if (a->generation == s->armed_generation) s->armed_generation = 0;
        s->capture_stopping = false;
        s->silent_frames = 0;
        s->silence_pending = false;
        return r;
    case MC100_ACT_HOLD: {
        if (s->deps.audio_control != NULL) {
            if (s->recording_generation) {
                r = s->deps.audio_control(s->deps.audio_control_ctx,
                                          MC100_SUPERVISOR_AUDIO_STOP,
                                          s->recording_generation);
                if (r != MC100_OK) return r;
            }
            if (s->armed_generation) {
                r = s->deps.audio_control(s->deps.audio_control_ctx,
                                          MC100_SUPERVISOR_AUDIO_STOP,
                                          s->armed_generation);
                if (r != MC100_OK) return r;
            }
        }
        mc100_writer_abandon(s->writer, a->detail);
        r = mc100_writer_release_handles(s->writer);
        mc100_result_t q = release_audio(s, s->recording_generation);
        if (r == MC100_OK) r = q;
        q = release_audio(s, s->armed_generation);
        if (r == MC100_OK) r = q;
        s->capture_stopping = true;
        if (r != MC100_OK)
            return r;
        s->writer_started = false;
        s->recording_generation = 0;
        s->armed_generation = 0;
        mc100_event_t e = {.id = MC100_EV_HELD, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_REPORT_FAULT:
    case MC100_ACT_BOOT:
    default: return MC100_OK;
    }
}

static mc100_result_t retry_owner_cleanup(mc100_supervisor_t *s)
{
    if (!s) return MC100_INVALID;
    if (s->hold_retry_pending) {
        mc100_result_t r = action(s, &s->hold_retry_action, now(s));
        if (r == MC100_OK)
            s->hold_retry_pending = false;
        return r;
    }
    if (s->release_retry_pending) {
        mc100_result_t r = action(s, &s->release_retry_action, now(s));
        if (r == MC100_OK)
            s->release_retry_pending = false;
        return r;
    }
    return MC100_OK;
}

static mc100_result_t pump(mc100_supervisor_t *s, const mc100_event_t *e)
{
    mc100_action_t actions[MC100_STATE_ACTION_CAPACITY];
    size_t count = 0;
    mc100_result_t r = mc100_state_step(s->state, e, actions, &count);
    if (r != MC100_OK) return r;
    mc100_result_t first = MC100_OK;
    for (size_t i = 0; i < count; ++i) {
        r = action(s, &actions[i], e->now_ms);
        if (r != MC100_OK) {
            if (actions[i].id == MC100_ACT_HOLD) {
                s->hold_retry_action = actions[i];
                s->hold_retry_pending = true;
            } else if (actions[i].id == MC100_ACT_RELEASE) {
                s->release_retry_action = actions[i];
                s->release_retry_pending = true;
            }
            first = r;
            /* Actions are an atomic state-machine batch.  Once an executor
             * fails it may have raised FAULT/HOLD and invalidated all later
             * actions from the old batch; never execute those stale commands.
             */
            break;
        }
    }
    return first;
}

mc100_result_t mc100_supervisor_post_event(mc100_supervisor_t *s,
                                           const mc100_event_t *event)
{
    if (!s || !event || !external_event_id(event->id)) return MC100_INVALID;

    mc100_event_t copy = *event;
    /* A monitor has no valid State generation.  FAULT is therefore always a
     * device-wide intent (for example ADC_INVALID), never a stale session
     * event supplied by an untrusted producer. */
    if (copy.id == MC100_EV_FAULT) copy.global = true;

    event_lock(s);
    if (nondroppable_event(&copy)) {
        /* Preserve the first critical intent until the owner observes it;
         * repeated samples are safely coalesced rather than overwritten. */
        if (!s->critical_pending) {
            s->critical_event = copy;
            s->critical_pending = true;
        }
        event_unlock(s);
        return MC100_OK;
    }
    if (s->event_count == MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY) {
        event_unlock(s);
        return MC100_FULL;
    }
    s->event_queue[s->event_tail] = copy;
    s->event_tail = (s->event_tail + 1u) %
                    MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY;
    ++s->event_count;
    event_unlock(s);
    return MC100_OK;
}

static bool take_external_event(mc100_supervisor_t *s, mc100_event_t *event)
{
    bool have = false;
    event_lock(s);
    if (s->critical_pending) {
        *event = s->critical_event;
        s->critical_event = (mc100_event_t){0};
        s->critical_pending = false;
        have = true;
    } else if (s->event_count != 0) {
        *event = s->event_queue[s->event_head];
        s->event_queue[s->event_head] = (mc100_event_t){0};
        s->event_head = (s->event_head + 1u) %
                        MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY;
        --s->event_count;
        have = true;
    }
    event_unlock(s);
    return have;
}

mc100_result_t mc100_supervisor_drain_events(mc100_supervisor_t *s)
{
    if (!s) return MC100_INVALID;
    mc100_result_t first = MC100_OK;
    /* A critical latch is intentionally dispatched before older FIFO items.
     * Apply those stale samples at the current owner watermark so priority
     * delivery cannot turn an otherwise harmless queued TICK into a backwards
     * monotonic-time error in State. */
    (void)now(s);
    mc100_event_t event;
    while (take_external_event(s, &event)) {
        event.now_ms = monotonic_event_time(s, event.now_ms);
        mc100_result_t result = pump(s, &event);
        if (result != MC100_OK && first == MC100_OK) first = result;
    }
    return first;
}

mc100_supervisor_t *mc100_supervisor_create(
    const mc100_supervisor_deps_t *deps)
{
    if (!deps || !deps->io || !deps->now_ms || !deps->boot_id) return NULL;
    mc100_supervisor_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->deps = *deps;
    s->state = mc100_state_create();
    s->audio = mc100_audio_create();
    s->writer = mc100_writer_create(deps->io, deps->io_ctx, deps->boot_id);
    if (!s->state || !s->audio || !s->writer) {
        mc100_writer_destroy(s->writer); mc100_audio_destroy(s->audio);
        mc100_state_destroy(s->state); free(s); return NULL;
    }
    mc100_assembler_init(&s->assembler, 0);
    return s;
}

mc100_result_t mc100_supervisor_rearm(mc100_supervisor_t *s)
{
    if (!s || mc100_state_get(s->state) != MC100_BOOT || !s->booted)
        return MC100_INVALID;
    if (s->writer_started || s->recording_generation || s->armed_generation ||
        s->capture_stopping)
        return MC100_NOT_READY;

    mc100_audio_stats_t audio_stats = {0};
    mc100_result_t r = mc100_audio_stats(s->audio, &audio_stats);
    if (r != MC100_OK) return r;
    if (audio_stats.current != 0) return MC100_NOT_READY;

    /* A publication may have been rotated immediately before the power hold.
     * Deliver it before replacing the writer so the writer-owned hand-off is
     * still exactly once across a recovery boundary. */
    r = publish_pending(s);
    if (r != MC100_OK) return r;
    r = mc100_writer_release_handles(s->writer);
    if (r != MC100_OK) return r;

    mc100_writer_t *writer = mc100_writer_create(
        s->deps.io, s->deps.io_ctx, s->deps.boot_id);
    mc100_audio_t *audio = mc100_audio_create();
    if (!writer || !audio) {
        mc100_writer_destroy(writer);
        mc100_audio_destroy(audio);
        return MC100_FULL;
    }
    mc100_writer_destroy(s->writer);
    mc100_audio_destroy(s->audio);
    s->writer = writer;
    s->audio = audio;
    mc100_assembler_init(&s->assembler, 0);
    s->armed_generation = 0;
    s->recording_generation = 0;
    s->silent_frames = 0;
    s->silence_pending = false;
    s->capture_stopping = false;
    s->writer_started = false;
    s->hold_retry_pending = false;
    s->release_retry_pending = false;
    s->booted = false;
    return MC100_OK;
}

void mc100_supervisor_destroy(mc100_supervisor_t *s)
{
    if (!s) return;
    /* Do not close/abandon here: reset recovery needs the part-file evidence. */
    mc100_writer_destroy(s->writer);
    mc100_audio_destroy(s->audio);
    mc100_state_destroy(s->state);
    free(s);
}

mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *s)
{
    if (!s || s->booted || mc100_state_get(s->state) != MC100_BOOT)
        return MC100_INVALID;
    if (!s->deps.battery_ready || !s->deps.battery_ready(s->deps.battery_ctx))
        return boot_failure(s, MC100_NOT_READY);
    uint64_t start = now(s);
    uint64_t deadline = start > UINT64_MAX - 30000 ? UINT64_MAX : start + 30000;
    mc100_recovery_report_t report;
    mc100_result_t r = mc100_recover(s->deps.io, s->deps.io_ctx, deadline,
                                     s->deps.now_ms, s->deps.clock_ctx,
                                     &report);
    if (r != MC100_OK) return boot_failure(s, r);
    r = mc100_writer_prepare(s->writer);
    if (r != MC100_OK) return boot_failure(s, r);
    mc100_writer_status_t st;
    r = mc100_writer_status(s->writer, &st);
    if (r != MC100_OK || st.prepared_slots < 2)
        return boot_failure(s, r == MC100_OK ? MC100_NOT_READY : r);
    if (!s->deps.driver_ready || !s->deps.driver_ready(s->deps.driver_ctx))
        return boot_failure(s, MC100_NOT_READY);
    mc100_event_t e = {.id = MC100_EV_READY, .now_ms = now(s), .detail = 2,
                       .global = true};
    r = pump(s, &e);
    if (r == MC100_OK) s->booted = true;
    else r = boot_failure(s, r);
    return r;
}

mc100_result_t mc100_supervisor_tick(mc100_supervisor_t *s)
{
    if (!s) return MC100_INVALID;
    if (!s->booted) {
        return mc100_state_get(s->state) == MC100_BOOT ?
                   mc100_supervisor_boot(s) : MC100_NOT_READY;
    }
    mc100_result_t first = MC100_OK;
    bool opened_this_tick = false;
    /* Cleanup commands are owner transactions.  If a close/release callback
     * transiently failed, retry it before accepting more capture work; state
     * has not received HELD/RELEASE acknowledgement yet. */
    mc100_result_t cleanup = retry_owner_cleanup(s);
    if (cleanup != MC100_OK)
        return cleanup;
    /* External monitor intent is dispatched by this owner task before the
     * periodic tick.  In particular, a latched CRITICAL must quiesce owners
     * before this iteration can ingest another PCM frame. */
    mc100_result_t external = mc100_supervisor_drain_events(s);
    if (external != MC100_OK) first = external;
    uint64_t at = now(s);
    mc100_event_t tick = {.id = MC100_EV_TICK, .now_ms = at};
    mc100_result_t r = pump(s, &tick);
    if (r != MC100_OK) first = r;
    mc100_state_id_t state = mc100_state_get(s->state);
    if ((state == MC100_LISTEN || state == MC100_RECORD) &&
        !s->capture_stopping && s->deps.pcm_read) {
        size_t count = 0;
        r = s->deps.pcm_read(s->deps.pcm_ctx, s->pcm_buffer,
                             sizeof(s->pcm_buffer), &count, 20);
        if (r == MC100_OK && count > sizeof(s->pcm_buffer))
            r = MC100_INVALID;
        if (r == MC100_OK)
            r = mc100_assembler_feed(&s->assembler, s->pcm_buffer, count,
                                     on_frame, s);
        if (r != MC100_OK) {
            if (first == MC100_OK) first = r;
            (void)raise_fault(s, r == MC100_IO ? MC100_FAULT_MIC_IO :
                              audio_reason(r));
        }
    }
    state = mc100_state_get(s->state);
    if (state == MC100_LISTEN && s->armed_generation) {
        mc100_audio_status_t st;
        r = mc100_audio_status(s->audio, s->armed_generation, &st);
        if (r == MC100_OK && st.error != MC100_OK) r = st.error;
        if (r != MC100_OK) {
            if (first == MC100_OK) first = r;
            (void)raise_fault(s, audio_reason(r));
        } else if (st.triggered) {
            mc100_snapshot_t snap;
            r = mc100_audio_snapshot(s->audio, s->armed_generation, &snap);
            if (r != MC100_OK) {
                if (first == MC100_OK) first = r;
                (void)raise_fault(s, audio_reason(r));
            } else {
                mc100_generation_t g = s->armed_generation;
                mc100_audio_status_t live = {0};
                r = mc100_audio_status(s->audio, g, &live);
                if (r != MC100_OK) {
                    if (first == MC100_OK) first = r;
                    (void)raise_fault(s, MC100_FAULT_INTERNAL_PROTOCOL);
                    return first;
                }
                uint64_t first_seq = snap.count ? snap.first_seq :
                                                    live.first_live_seq;
                mc100_event_t e = {.id = MC100_EV_TRIGGER, .generation = g,
                                   .seq = first_seq, .now_ms = at,
                                   .seq_valid = true};
                s->armed_generation = 0;
                r = pump(s, &e);
                if (r == MC100_OK)
                    opened_this_tick = true;
                if (r != MC100_OK && first == MC100_OK) first = r;
            }
        }
    }
    state = mc100_state_get(s->state);
    if (state == MC100_RECORD && !s->capture_stopping && !opened_this_tick) {
        r = drain(s, s->recording_generation);
        if (r != MC100_OK) {
            if (first == MC100_OK) first = r;
            (void)raise_fault(s, storage_reason(r));
        }
        if (s->silence_pending && !s->capture_stopping) {
            s->silence_pending = false;
            mc100_event_t e = {.id = MC100_EV_SILENCE_END,
                               .generation = s->recording_generation,
                               .now_ms = at};
            r = pump(s, &e);
            if (r != MC100_OK && first == MC100_OK) first = r;
        }
        if (mc100_state_get(s->state) == MC100_RECORD &&
            !s->capture_stopping && s->writer_started) {
            r = mc100_writer_checkpoint(s->writer, at);
            if (r != MC100_OK && r != MC100_NOT_READY) {
                if (first == MC100_OK) first = r;
                (void)raise_fault(s, storage_reason(r));
            }
        }
    }
    return first;
}

mc100_state_id_t mc100_supervisor_state(const mc100_supervisor_t *s)
{ return s ? mc100_state_get(s->state) : MC100_FAULT; }

mc100_result_t mc100_supervisor_writer_status(
    const mc100_supervisor_t *s, mc100_writer_status_t *status)
{ return !s || !status ? MC100_INVALID : mc100_writer_status(s->writer, status); }
