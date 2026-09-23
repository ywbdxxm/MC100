#include "mc100_supervisor.h"

#include <inttypes.h>
#include <stdio.h>
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
};

static uint64_t now(const mc100_supervisor_t *s)
{ return s->deps.now_ms(s->deps.clock_ctx); }

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

static mc100_result_t drain(mc100_supervisor_t *s, mc100_generation_t g)
{
    for (;;) {
        mc100_packet_t p;
        mc100_result_t r = mc100_audio_pop(s->audio, &p);
        if (r == MC100_NOT_READY) return MC100_OK;
        if (r != MC100_OK || p.generation != g)
            return r == MC100_OK ? MC100_CORRUPT : r;
        if (!s->writer_started) {
            r = writer_start(s, g, p.frame.seq);
            if (r != MC100_OK) return r;
        }
        r = mc100_writer_append(s->writer, &p);
        if (r != MC100_OK) return r;
    }
}

static bool final_name(const mc100_supervisor_t *s,
                       const mc100_writer_status_t *st,
                       char out[MC100_PATH_BYTES])
{
    static const char hex[] = "0123456789abcdef";
    char id[33];
    for (size_t i = 0; i < 16; ++i) {
        id[i * 2] = hex[s->deps.boot_id[i] >> 4];
        id[i * 2 + 1] = hex[s->deps.boot_id[i] & 15];
    }
    id[32] = 0;
    int n = snprintf(out, MC100_PATH_BYTES, "%s_%" PRIu64 "_%" PRIu32 ".wav",
                     id, st->generation, st->segment_index);
    return n > 0 && n < MC100_PATH_BYTES;
}

static mc100_result_t action(mc100_supervisor_t *s,
                             const mc100_action_t *a, uint64_t at)
{
    mc100_result_t r;
    switch (a->id) {
    case MC100_ACT_ARM: {
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
        mc100_snapshot_t snap;
        r = mc100_audio_snapshot(s->audio, a->generation, &snap);
        if (r == MC100_OK && snap.count)
            r = writer_start(s, a->generation, snap.first_seq);
        mc100_packet_t p = {.generation = a->generation};
        for (uint16_t i = 0; r == MC100_OK && i < snap.count; ++i) {
            r = mc100_audio_snapshot_frame(s->audio, &snap, i, &p.frame);
            if (r == MC100_OK) r = mc100_writer_append(s->writer, &p);
        }
        mc100_result_t released = mc100_audio_snapshot_release(
            s->audio, a->generation);
        if (r == MC100_OK) r = released;
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
        r = mc100_audio_stop(s->audio, a->generation, &cutoff);
        mc100_audio_status_t st = {0};
        mc100_result_t observed = mc100_audio_status(s->audio, a->generation,
                                                      &st);
        if (r == MC100_OK && observed != MC100_OK) r = observed;
        s->capture_stopping = true;
        mc100_event_t e = {.id = MC100_EV_CAPTURE_STOPPED,
                           .generation = a->generation, .seq = cutoff,
                           .now_ms = at,
                           .seq_valid = observed == MC100_OK && st.cutoff_valid};
        mc100_result_t t = pump(s, &e);
        return r != MC100_OK ? r : t;
    }
    case MC100_ACT_CLOSE_THROUGH: {
        r = drain(s, a->generation);
        mc100_writer_status_t st = {0};
        if (r == MC100_OK) r = mc100_writer_status(s->writer, &st);
        char name[MC100_PATH_BYTES] = {0};
        bool normal = a->detail == 0 && a->seq_valid;
        if (r == MC100_OK && a->seq_valid) {
            if (!st.active) {
                /* A writer-owned admission FULL may already have published the
                 * incident; never issue a second close. */
                if (a->detail != MC100_FAULT_STORAGE_FULL ||
                    st.latched_reason == 0) r = MC100_NOT_READY;
            } else if (!st.has_frames || st.generation != a->generation ||
                       st.last_seq != a->seq)
                r = MC100_CORRUPT;
            else {
                if (normal && !final_name(s, &st, name)) r = MC100_INVALID;
                if (r == MC100_OK)
                    r = mc100_writer_close_through(s->writer, a->generation,
                                                   a->seq, a->detail);
            }
        } else if (r == MC100_OK && s->writer_started) {
            mc100_writer_abandon(s->writer, a->detail);
            r = mc100_writer_release_handles(s->writer);
        }
        if (r != MC100_OK) {
            (void)raise_fault(s, storage_reason(r));
            return r;
        }
        s->writer_started = false;
        if (normal && s->deps.upload.notify_closed != NULL)
            s->deps.upload.notify_closed(s->deps.upload.ctx, name);
        mc100_event_t e = {.id = MC100_EV_CLOSED, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_RELEASE:
        r = release_audio(s, a->generation);
        if (a->generation == s->recording_generation)
            s->recording_generation = 0;
        if (a->generation == s->armed_generation) s->armed_generation = 0;
        s->capture_stopping = false;
        s->silent_frames = 0;
        s->silence_pending = false;
        return r;
    case MC100_ACT_HOLD: {
        mc100_writer_abandon(s->writer, a->detail);
        r = mc100_writer_release_handles(s->writer);
        mc100_result_t q = release_audio(s, s->recording_generation);
        if (r == MC100_OK) r = q;
        q = release_audio(s, s->armed_generation);
        if (r == MC100_OK) r = q;
        s->writer_started = false;
        s->recording_generation = 0;
        s->armed_generation = 0;
        s->capture_stopping = true;
        if (r != MC100_OK) return r;
        mc100_event_t e = {.id = MC100_EV_HELD, .generation = a->generation,
                           .now_ms = at};
        return pump(s, &e);
    }
    case MC100_ACT_REPORT_FAULT:
    case MC100_ACT_BOOT:
    default: return MC100_OK;
    }
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
        if (r != MC100_OK && first == MC100_OK) first = r;
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
    if (!s || !s->booted) return MC100_NOT_READY;
    mc100_result_t first = MC100_OK;
    bool opened_this_tick = false;
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
                mc100_event_t e = {.id = MC100_EV_TRIGGER, .generation = g,
                                   .seq = snap.first_seq, .now_ms = at,
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
