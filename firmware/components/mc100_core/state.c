#include "mc100_state.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    mc100_generation_t generation;
    uint64_t first, cutoff, operation_since, stop_since;
    bool used, opening, opened, closing, stop_wait, stopped, cutoff_valid, canceled_grant;
} session_t;

struct mc100_state {
    mc100_state_id_t state;
    session_t current, pending;
    mc100_generation_t serial, grant, hold_token;
    uint64_t now, arm_since, minimum_first, hold_since, low_since;
    bool have_time, arm_wait, armed, minimum_valid;
    bool hold_wait, no_writes, low_budget, quiesced;
    mc100_fault_reason_t first_fault;
};
typedef struct { mc100_action_t action[8]; size_t count; bool full; } output_t;

static void emit(output_t *o, mc100_action_id_t id, mc100_generation_t g,
    uint64_t seq, uint32_t detail, bool valid)
{
    mc100_action_t a = {0};
    if (o->count == MC100_STATE_ACTION_CAPACITY) { o->full = true; return; }
    a.id = id; a.generation = g; a.seq = seq; a.detail = detail; a.seq_valid = valid;
    o->action[o->count++] = a;
}

static mc100_generation_t token(mc100_state_t *s)
{
    if (s->serial == UINT64_MAX) return 0;
    return ++s->serial;
}

static void stop(mc100_state_t *s, session_t *c, output_t *o);

/* A grant can already own a frozen bank even when its TRIGGER is still queued.
 * Move it into the existing spare context before revoking admission. */
static void retain_grant(mc100_state_t *s)
{
    if (s->grant) {
        s->pending.used = true;
        s->pending.canceled_grant = true;
        s->pending.generation = s->grant;
        s->grant = 0;
    }
    s->arm_wait = s->armed = false;
}

static void hold(mc100_state_t *s, output_t *o, mc100_hold_reason_t reason)
{
    s->hold_wait = true;
    s->hold_since = s->now;
    s->hold_token = token(s);
    retain_grant(s);
    emit(o, MC100_ACT_HOLD, s->hold_token, 0, (uint32_t)reason, false);
}

static void fault(mc100_state_t *s, output_t *o, mc100_fault_reason_t reason)
{
    bool safe_prefix = reason == MC100_FAULT_MIC_IO ||
        reason == MC100_FAULT_QUEUE_OVERFLOW || reason == MC100_FAULT_STORAGE_FULL;
    if (!s->first_fault) {
        s->first_fault = reason;
        emit(o, MC100_ACT_REPORT_FAULT, s->current.generation, 0, (uint32_t)reason, false);
    }
    s->state = MC100_FAULT;
    if (s->no_writes) return;
    retain_grant(s);
    if (safe_prefix) {
        stop(s, &s->current, o);
        stop(s, &s->pending, o);
        return;
    }
    s->no_writes = true; s->low_budget = false;
    /* Preserve both contexts until owners acknowledge they no longer refer to
     * them. Token 0 is reserved for emergency quiescence at serial exhaustion. */
    hold(s, o, MC100_HOLD_FAULT_NO_WRITES);
}

static void arm(mc100_state_t *s, output_t *o)
{
    s->grant = token(s);
    if (!s->grant) { fault(s, o, MC100_FAULT_INTERNAL_PROTOCOL); return; }
    s->arm_wait = true;
    s->armed = false;
    s->arm_since = s->now;
    emit(o, MC100_ACT_ARM, s->grant, s->minimum_first,
        MC100_ARM_SILENCE_FRAMES, s->minimum_valid);
}

static void stop(mc100_state_t *s, session_t *c, output_t *o)
{
    if (!c->used || c->stop_wait || c->stopped) return;
    c->stop_wait = true;
    c->stop_since = s->now;
    emit(o, MC100_ACT_STOP_CAPTURE, c->generation, 0, 0, false);
}

static void release(session_t *c, output_t *o)
{
    if (c->used) emit(o, MC100_ACT_RELEASE, c->generation, 0, 0, false);
    memset(c, 0, sizeof(*c));
}

static void open_current(mc100_state_t *s, output_t *o)
{
    s->current.opening = true;
    s->current.operation_since = s->now;
    emit(o, MC100_ACT_OPEN, s->current.generation, s->current.first, 0, true);
}

static bool expired(uint64_t now, uint64_t start, uint64_t duration)
{ return now - start >= duration; }

static bool check_timeout(mc100_state_t *s, output_t *o)
{
    const session_t *c[2] = { &s->current, &s->pending };
    size_t i;
    if ((s->state == MC100_FAULT && s->no_writes) || s->state == MC100_LOW_BAT_HOLD) return false;
    if ((s->arm_wait && expired(s->now, s->arm_since, MC100_CONTROL_TIMEOUT_MS)) ||
        (s->hold_wait && expired(s->now, s->hold_since, MC100_CONTROL_TIMEOUT_MS))) {
        fault(s, o, MC100_FAULT_CONTROL_TIMEOUT); return true;
    }
    for (i = 0; i < 2; ++i) {
        if (c[i]->stop_wait && expired(s->now, c[i]->stop_since, MC100_CONTROL_TIMEOUT_MS)) {
            fault(s, o, MC100_FAULT_CONTROL_TIMEOUT); return true;
        }
        if ((c[i]->opening || c[i]->closing) &&
            expired(s->now, c[i]->operation_since, MC100_STORAGE_TIMEOUT_MS)) {
            fault(s, o, MC100_FAULT_STORAGE_TIMEOUT); return true;
        }
    }
    if (s->low_budget && expired(s->now, s->low_since, MC100_STORAGE_TIMEOUT_MS)) {
        fault(s, o, MC100_FAULT_STORAGE_TIMEOUT); return true;
    }
    return false;
}

/* Advance only effects whose prerequisites are acknowledged. No storage OPEN
 * for the successor occurs until the previous CLOSED has released its context. */
static void advance(mc100_state_t *s, output_t *o)
{
    session_t *c = &s->current;
    bool finalizing = s->state == MC100_LOW_BAT || s->state == MC100_FAULT;
    if (s->no_writes || s->hold_wait) return;
    if (finalizing && s->pending.used && s->pending.stopped)
        release(&s->pending, o);
    if (c->used && c->opened && c->stopped && !c->closing) {
        c->closing = true;
        c->operation_since = s->now;
        emit(o, MC100_ACT_CLOSE_THROUGH, c->generation, c->cutoff,
            s->state == MC100_FAULT ? (uint32_t)s->first_fault : 0, c->cutoff_valid);
        if (c->cutoff_valid) {
            if (c->cutoff == UINT64_MAX) {
                fault(s, o, MC100_FAULT_INTERNAL_PROTOCOL); return;
            }
            s->minimum_first = c->cutoff + 1;
            s->minimum_valid = true;
        }
        if (s->state == MC100_RECORD && !s->pending.used && !s->grant) arm(s, o);
    }
    if (finalizing && !c->used && !s->pending.used) {
        s->low_budget = false;
        if (s->state == MC100_FAULT) {
            s->no_writes = true;
            hold(s, o, MC100_HOLD_FAULT_NO_WRITES);
        } else hold(s, o, MC100_HOLD_LOW);
    }
}

static session_t *find(mc100_state_t *s, mc100_generation_t g)
{
    if (s->current.used && s->current.generation == g) return &s->current;
    if (s->pending.used && s->pending.generation == g) return &s->pending;
    return NULL;
}

static void handle(mc100_state_t *s, const mc100_event_t *e, output_t *o)
{
    session_t *c = find(s, e->generation);
    switch (e->id) {
    case MC100_EV_READY:
        if (s->state != MC100_BOOT) break;
        if (e->detail < 2) { fault(s, o, MC100_FAULT_STORAGE_FULL); break; }
        s->state = MC100_LISTEN;
        arm(s, o);
        break;
    case MC100_EV_ARMED:
        if (s->arm_wait && e->generation == s->grant) {
            s->arm_wait = false; s->armed = true;
        }
        break;
    case MC100_EV_TRIGGER:
        if (!s->armed || e->generation != s->grant ||
            (s->state != MC100_LISTEN && s->state != MC100_RECORD)) break;
        if (s->pending.used || (s->current.used && !s->current.closing)) {
            fault(s, o, MC100_FAULT_INTERNAL_PROTOCOL); break;
        }
        if (s->minimum_valid && e->seq < s->minimum_first) {
            fault(s, o, MC100_FAULT_INTERNAL_PROTOCOL); break;
        }
        c = s->current.used ? &s->pending : &s->current;
        memset(c, 0, sizeof(*c));
        c->used = true; c->generation = e->generation; c->first = e->seq;
        s->grant = 0; s->armed = false; s->state = MC100_RECORD;
        if (c == &s->current) open_current(s, o);
        break;
    case MC100_EV_OPENED:
        if (c == &s->current && c->opening && !s->no_writes) {
            c->opening = false; c->opened = true;
        }
        break;
    case MC100_EV_SILENCE_END:
        if (!s->no_writes && c) stop(s, c, o);
        break;
    case MC100_EV_CAPTURE_STOPPED:
        if (c && c->stop_wait && !s->no_writes) {
            c->stop_wait = false; c->stopped = true;
            c->cutoff = e->seq; c->cutoff_valid = e->seq_valid;
            if (!c->canceled_grant && e->seq_valid && e->seq < c->first)
                fault(s, o, MC100_FAULT_INTERNAL_PROTOCOL);
        }
        break;
    case MC100_EV_CLOSED:
        if (c != &s->current || !c->closing || s->no_writes) break;
        release(c, o);
        if (s->state == MC100_RECORD) {
            if (s->pending.used) {
                s->current = s->pending;
                memset(&s->pending, 0, sizeof(s->pending));
                open_current(s, o);
            } else s->state = MC100_LISTEN;
        }
        break;
    case MC100_EV_LOW:
        if (s->state == MC100_LOW_BAT || s->state == MC100_LOW_BAT_HOLD ||
            s->state == MC100_FAULT) break;
        s->state = MC100_LOW_BAT;
        retain_grant(s);
        s->low_budget = true; s->low_since = s->now;
        stop(s, &s->current, o); stop(s, &s->pending, o);
        break;
    case MC100_EV_CRITICAL:
        if (s->state == MC100_LOW_BAT_HOLD || s->no_writes) break;
        if (s->state != MC100_FAULT) s->state = MC100_LOW_BAT;
        s->no_writes = true; s->low_budget = false;
        s->current.stop_wait = s->pending.stop_wait = false;
        s->current.opening = s->current.closing = false;
        hold(s, o, MC100_HOLD_CRITICAL_NO_WRITES);
        break;
    case MC100_EV_HELD:
        if (!s->hold_wait || e->generation != s->hold_token) break;
        s->hold_wait = false; s->quiesced = true;
        release(&s->current, o); release(&s->pending, o);
        if (s->state == MC100_LOW_BAT) s->state = MC100_LOW_BAT_HOLD;
        break;
    case MC100_EV_RECOVERED_POWER:
        if (s->state != MC100_LOW_BAT_HOLD || !s->quiesced) break;
        s->state = MC100_BOOT; s->no_writes = s->quiesced = false;
        s->minimum_valid = false;
        emit(o, MC100_ACT_BOOT, 0, 0, 0, false);
        break;
    case MC100_EV_FAULT:
        if (e->global || c || (s->grant && e->generation == s->grant))
            fault(s, o, (mc100_fault_reason_t)e->detail);
        break;
    case MC100_EV_TICK:
    case MC100_EV_ROTATED:
        /* Storage owns segment_index; rotation does not end the session. */
        break;
    }
    advance(s, o);
}

mc100_state_t *mc100_state_create(void) { return calloc(1, sizeof(mc100_state_t)); }
void mc100_state_destroy(mc100_state_t *s) { free(s); }
mc100_state_id_t mc100_state_get(const mc100_state_t *s)
{ return s ? s->state : MC100_FAULT; }
mc100_result_t mc100_state_step(mc100_state_t *s, const mc100_event_t *e,
    mc100_action_t out[8], size_t *count)
{
    mc100_state_t next;
    output_t o = {0};
    mc100_result_t result = MC100_OK;
    if (count) *count = 0;
    if (!s || !e || !out || !count || e->id < MC100_EV_READY || e->id > MC100_EV_HELD ||
        (s->have_time && e->now_ms < s->now) ||
        (e->id == MC100_EV_TRIGGER && !e->seq_valid) ||
        (e->id == MC100_EV_FAULT && (e->detail < MC100_FAULT_MIC_IO ||
            e->detail > MC100_FAULT_STORAGE_TIMEOUT))) return MC100_INVALID;
    next = *s;
    next.now = e->now_ms; next.have_time = true;
    if (!check_timeout(&next, &o)) handle(&next, e, &o);
    if (o.full) {
        /* Discard the entire attempted transition and substitute one bounded
         * fail-safe transaction. The caller never receives a partial batch. */
        next = *s; next.now = e->now_ms; next.have_time = true;
        memset(&o, 0, sizeof(o));
        fault(&next, &o, MC100_FAULT_INTERNAL_PROTOCOL);
        result = MC100_CORRUPT;
    }
    *s = next;
    memcpy(out, o.action, o.count * sizeof(*out));
    *count = o.count;
    return result;
}
