#include "mc100_state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static mc100_action_t a[8];
static size_t n;
static void event(mc100_state_t *s, mc100_event_id_t id,
    uint64_t gen, uint64_t seq, uint64_t time, uint32_t detail, bool valid)
{
    mc100_event_t e = {0};
    e.id = id; e.generation = gen; e.seq = seq; e.now_ms = time;
    e.detail = detail; e.seq_valid = valid;
    assert(mc100_state_step(s, &e, a, &n) == MC100_OK);
    assert(n <= 8);
}
static mc100_generation_t boot(mc100_state_t *s, uint64_t t)
{
    assert(mc100_state_get(s) == MC100_BOOT);
    event(s, MC100_EV_READY, 0, 0, t, 2, false);
    assert(mc100_state_get(s) == MC100_LISTEN);
    assert(n == 1 && a[0].id == MC100_ACT_ARM);
    assert(a[0].detail == 750 && a[0].generation != 0);
    return a[0].generation;
}
static mc100_generation_t record(mc100_state_t *s)
{
    mc100_generation_t g = boot(s, 0);
    event(s, MC100_EV_ARMED, g, 0, 1, 0, false);
    event(s, MC100_EV_TRIGGER, g, 0, 2, 0, true);
    assert(n == 1 && a[0].id == MC100_ACT_OPEN && a[0].seq_valid);
    event(s, MC100_EV_OPENED, g, 0, 3, 0, false);
    assert(mc100_state_get(s) == MC100_RECORD);
    return g;
}
static void close_drain_and_retrigger(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), next;
    event(s, MC100_EV_SILENCE_END, g, 999, 4, 0, true);
    assert(n == 1 && a[0].id == MC100_ACT_STOP_CAPTURE);
    assert(mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_CLOSED, g, 0, 5, 0, false);
    assert(n == 0 && mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 1000, 6, 0, true);
    assert(n == 2 && a[0].id == MC100_ACT_CLOSE_THROUGH);
    assert(a[0].generation == g && a[0].seq == 1000 && a[0].seq_valid);
    assert(a[1].id == MC100_ACT_ARM && a[1].seq == 1001 && a[1].seq_valid);
    next = a[1].generation;
    event(s, MC100_EV_CAPTURE_STOPPED, g, 2000, 7, 0, true);
    assert(n == 0);
    event(s, MC100_EV_ARMED, next, 0, 8, 0, false);
    event(s, MC100_EV_TRIGGER, next, 1001, 9, 0, true);
    assert(n == 0 && mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_TRIGGER, next, 1100, 10, 0, true);
    assert(n == 0);
    event(s, MC100_EV_CLOSED, g, 0, 11, 0, false);
    assert(n == 2 && a[0].id == MC100_ACT_RELEASE && a[0].generation == g);
    assert(a[1].id == MC100_ACT_OPEN && a[1].generation == next && a[1].seq == 1001);
    assert(mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_CLOSED, g, 0, 12, 0, false);
    assert(n == 0 && mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_TRIGGER, g, 1, 13, 0, true);
    assert(n == 0);
    event(s, MC100_EV_OPENED, next, 0, 14, 0, false);
    event(s, MC100_EV_SILENCE_END, next, 0, 15, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, next, 1100, 16, 0, true);
    assert(n == 2);
    event(s, MC100_EV_CLOSED, next, 0, 17, 0, false);
    assert(n == 1 && a[0].generation == next && mc100_state_get(s) == MC100_LISTEN);
    mc100_state_destroy(s);
}
static void armed_required_and_timeouts(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = boot(s, 0);
    event(s, MC100_EV_TRIGGER, g, 0, 1, 0, true);
    assert(n == 0 && mc100_state_get(s) == MC100_LISTEN);
    event(s, MC100_EV_READY, 0, 0, 199, 2, false);
    event(s, MC100_EV_TICK, 0, 0, 200, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT && n == 2);
    assert(a[0].detail == MC100_FAULT_CONTROL_TIMEOUT);
    mc100_state_destroy(s);

    s = mc100_state_create(); g = record(s);
    event(s, MC100_EV_SILENCE_END, g, 0, 4, 0, false);
    event(s, MC100_EV_SILENCE_END, g, 0, 203, 0, false);
    event(s, MC100_EV_TICK, 0, 0, 204, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT);
    assert(a[0].detail == MC100_FAULT_CONTROL_TIMEOUT);
    mc100_state_destroy(s);

    s = mc100_state_create(); g = record(s);
    event(s, MC100_EV_SILENCE_END, g, 0, 4, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 5, 5, 0, true);
    g = a[1].generation;
    event(s, MC100_EV_ARMED, g, 0, 6, 0, false);
    event(s, MC100_EV_TICK, 0, 0, 1504, 0, false);
    assert(mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_TICK, 0, 0, 1505, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT);
    assert(a[0].detail == MC100_FAULT_STORAGE_TIMEOUT);
    mc100_state_destroy(s);
}
static void low_requires_owner_ack(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t token;
    event(s, MC100_EV_LOW, 0, 0, 0, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_HOLD);
    assert(mc100_state_get(s) == MC100_LOW_BAT);
    token = a[0].generation;
    event(s, MC100_EV_HELD, token + 1, 0, 1, 0, false);
    assert(mc100_state_get(s) == MC100_LOW_BAT);
    event(s, MC100_EV_RECOVERED_POWER, 0, 0, 2, 0, false);
    assert(n == 0);
    event(s, MC100_EV_HELD, token, 0, 3, 0, false);
    assert(mc100_state_get(s) == MC100_LOW_BAT_HOLD);
    event(s, MC100_EV_HELD, token, 0, 4, 0, false);
    assert(n == 0);
    event(s, MC100_EV_RECOVERED_POWER, 0, 0, 5, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_BOOT && mc100_state_get(s) == MC100_BOOT);
    mc100_state_destroy(s);
}
static void low_drain_and_pending_release(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), next, hold;
    event(s, MC100_EV_SILENCE_END, g, 0, 4, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 10, 5, 0, true);
    next = a[1].generation;
    event(s, MC100_EV_ARMED, next, 0, 6, 0, false);
    event(s, MC100_EV_TRIGGER, next, 11, 7, 0, true);
    event(s, MC100_EV_LOW, 0, 0, 8, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_STOP_CAPTURE && a[0].generation == next);
    event(s, MC100_EV_CAPTURE_STOPPED, next, 12, 9, 0, true);
    assert(n == 1 && a[0].id == MC100_ACT_RELEASE && a[0].generation == next);
    event(s, MC100_EV_CLOSED, g, 0, 10, 0, false);
    assert(n == 2 && a[0].id == MC100_ACT_RELEASE && a[1].id == MC100_ACT_HOLD);
    hold = a[1].generation;
    assert(mc100_state_get(s) == MC100_LOW_BAT);
    event(s, MC100_EV_HELD, hold, 0, 11, 0, false);
    assert(mc100_state_get(s) == MC100_LOW_BAT_HOLD);
    mc100_state_destroy(s);
}
static void critical_forbids_close_and_fault_scope(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), hold;
    event(s, MC100_EV_FAULT, g + 1, 0, 4, MC100_FAULT_MIC_IO, false);
    assert(n == 0 && mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_CRITICAL, 0, 0, 5, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_HOLD);
    assert(a[0].detail == MC100_HOLD_CRITICAL_NO_WRITES);
    hold = a[0].generation;
    event(s, MC100_EV_CAPTURE_STOPPED, g, 1, 6, 0, true);
    assert(n == 0);
    event(s, MC100_EV_HELD, hold, 0, 7, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_RELEASE);
    assert(mc100_state_get(s) == MC100_LOW_BAT_HOLD);
    mc100_state_destroy(s);
}
static void no_data_and_open_deadline(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = boot(s, 0);
    event(s, MC100_EV_ARMED, g, 0, 1, 0, false);
    event(s, MC100_EV_TRIGGER, g, 0, 2, 0, true);
    event(s, MC100_EV_SILENCE_END, g, 0, 3, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 0, 4, 0, false);
    assert(n == 0); /* Opening must finish before close is dispatched. */
    event(s, MC100_EV_OPENED, g, 0, 5, 0, false);
    assert(n == 2 && a[0].id == MC100_ACT_CLOSE_THROUGH && !a[0].seq_valid);
    assert(a[1].id == MC100_ACT_ARM && !a[1].seq_valid);
    mc100_state_destroy(s);
    s = mc100_state_create(); g = boot(s, 0);
    event(s, MC100_EV_ARMED, g, 0, 1, 0, false);
    event(s, MC100_EV_TRIGGER, g, 0, 2, 0, true);
    event(s, MC100_EV_TRIGGER, g, 0, 1501, 0, true);
    assert(mc100_state_get(s) == MC100_RECORD);
    event(s, MC100_EV_TICK, 0, 0, 1502, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT && a[0].detail == MC100_FAULT_STORAGE_TIMEOUT);
    mc100_state_destroy(s);
}
static void monotonic_and_saturation(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = boot(s, UINT64_MAX - 199);
    mc100_event_t e = {0};
    e.id = MC100_EV_ARMED; e.generation = g; e.now_ms = UINT64_MAX - 200;
    n = 7;
    assert(mc100_state_step(s, &e, a, &n) == MC100_INVALID && n == 0);
    event(s, MC100_EV_TICK, 0, 0, UINT64_MAX, 0, false);
    assert(mc100_state_get(s) == MC100_LISTEN);
    event(s, MC100_EV_ARMED, g, 0, UINT64_MAX, 0, false);
    assert(mc100_state_get(s) == MC100_LISTEN);
    mc100_state_destroy(s);
}
static void low_active_timeout_and_hold_timeout(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), h;
    event(s, MC100_EV_LOW, 0, 0, 4, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_STOP_CAPTURE);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 0, 5, 0, true);
    assert(n == 1 && a[0].id == MC100_ACT_CLOSE_THROUGH && a[0].seq_valid);
    event(s, MC100_EV_CLOSED, g, 0, 6, 0, false);
    assert(n == 2 && a[1].id == MC100_ACT_HOLD);
    h = a[1].generation;
    event(s, MC100_EV_LOW, 0, 0, 205, 0, false);
    event(s, MC100_EV_HELD, h + 1, 0, 206, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT && a[0].detail == MC100_FAULT_CONTROL_TIMEOUT);
    mc100_state_destroy(s);
    s = mc100_state_create(); g = record(s);
    event(s, MC100_EV_LOW, 0, 0, 4, 0, false);
    event(s, MC100_EV_LOW, 0, 0, 203, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 0, 204, 0, true);
    assert(mc100_state_get(s) == MC100_FAULT && n == 2);
    assert(a[0].detail == MC100_FAULT_CONTROL_TIMEOUT);
    assert(a[1].id == MC100_ACT_HOLD && a[1].detail == MC100_HOLD_FAULT_NO_WRITES);
    mc100_state_destroy(s);
    s = mc100_state_create(); g = record(s);
    event(s, MC100_EV_LOW, 0, 0, 4, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 0, 203, 0, true);
    event(s, MC100_EV_TICK, 0, 0, 1503, 0, false);
    assert(mc100_state_get(s) == MC100_LOW_BAT);
    event(s, MC100_EV_TICK, 0, 0, 1504, 0, false);
    assert(mc100_state_get(s) == MC100_FAULT && a[0].detail == MC100_FAULT_STORAGE_TIMEOUT);
    mc100_state_destroy(s);
}
static void pending_silence_and_global_fault(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), next, h;
    mc100_event_t e = {0};
    event(s, MC100_EV_SILENCE_END, g, 0, 4, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 5, 5, 0, true);
    next = a[1].generation;
    event(s, MC100_EV_ARMED, next, 0, 6, 0, false);
    event(s, MC100_EV_TRIGGER, next, 6, 7, 0, true);
    event(s, MC100_EV_SILENCE_END, next, 0, 8, 0, false);
    assert(n == 1 && a[0].generation == next);
    event(s, MC100_EV_CAPTURE_STOPPED, next, 10, 9, 0, true);
    assert(n == 0);
    event(s, MC100_EV_CLOSED, g, 0, 10, 0, false);
    assert(n == 2 && a[1].id == MC100_ACT_OPEN && a[1].generation == next);
    event(s, MC100_EV_OPENED, next, 0, 11, 0, false);
    assert(n == 2 && a[0].id == MC100_ACT_CLOSE_THROUGH && a[0].seq == 10);
    event(s, MC100_EV_FAULT, 0, 0, 12, MC100_FAULT_ADC_INVALID, false);
    assert(n == 0 && mc100_state_get(s) == MC100_RECORD);
    e.id = MC100_EV_FAULT; e.global = true; e.detail = MC100_FAULT_ADC_INVALID; e.now_ms = 13;
    assert(mc100_state_step(s, &e, a, &n) == MC100_OK);
    assert(n == 2 && a[0].id == MC100_ACT_REPORT_FAULT && a[1].id == MC100_ACT_HOLD);
    h = a[1].generation;
    event(s, MC100_EV_HELD, h, 0, 14, 0, false);
    assert(n == 1 && a[0].id == MC100_ACT_RELEASE && a[0].generation == next);
    assert(mc100_state_get(s) == MC100_FAULT);
    event(s, MC100_EV_RECOVERED_POWER, 0, 0, 15, 0, false);
    assert(n == 0 && mc100_state_get(s) == MC100_FAULT);
    mc100_state_destroy(s);
}
static uint32_t random_next(uint32_t *seed)
{ *seed = *seed * UINT32_C(1664525) + UINT32_C(1013904223); return *seed; }
static void fault_retains_both_contexts_until_quiescent(void)
{
    mc100_state_t *s = mc100_state_create();
    uint64_t g = record(s), next, h;
    event(s, MC100_EV_SILENCE_END, g, 0, 4, 0, false);
    event(s, MC100_EV_CAPTURE_STOPPED, g, 10, 5, 0, true);
    next = a[1].generation;
    event(s, MC100_EV_ARMED, next, 0, 6, 0, false);
    event(s, MC100_EV_TRIGGER, next, 11, 7, 0, true);
    event(s, MC100_EV_FAULT, next, 0, 8, MC100_FAULT_QUEUE_OVERFLOW, false);
    assert(n == 2 && a[0].id == MC100_ACT_REPORT_FAULT && a[1].id == MC100_ACT_HOLD);
    h = a[1].generation;
    event(s, MC100_EV_CLOSED, g, 0, 9, 0, false);
    assert(n == 0 && mc100_state_get(s) == MC100_FAULT);
    event(s, MC100_EV_HELD, h - 1, 0, 10, 0, false);
    assert(n == 0);
    event(s, MC100_EV_HELD, h, 0, 11, 0, false);
    assert(n == 2 && a[0].id == MC100_ACT_RELEASE && a[0].generation == g);
    assert(a[1].id == MC100_ACT_RELEASE && a[1].generation == next);
    event(s, MC100_EV_HELD, h, 0, 12, 0, false);
    assert(n == 0);
    mc100_state_destroy(s);
}
static void deterministic_bounded_replay(void)
{
    uint32_t seed = UINT32_C(0xc100b002);
    size_t run, k, i;
    for (run = 0; run < 64; ++run) {
        mc100_state_t *s[2] = {mc100_state_create(), mc100_state_create()};
        uint64_t now = 0;
        for (k = 0; k < 256; ++k) {
            struct { uint64_t before; mc100_action_t out[8]; uint64_t after; } b[2];
            mc100_event_t e = {0};
            mc100_result_t r[2];
            size_t count[2];
            e.id = (mc100_event_id_t)(random_next(&seed) % 14);
            e.generation = random_next(&seed) % 8;
            e.seq = random_next(&seed) % 1000;
            now += random_next(&seed) % 32; e.now_ms = now;
            e.seq_valid = true; e.global = (random_next(&seed) % 8) == 0;
            e.detail = 2;
            if (k == 0) { e.id = MC100_EV_READY; e.now_ms = now; }
            for (i = 0; i < 2; ++i) {
                b[i].before = UINT64_C(0xfeed12345678beef);
                b[i].after = UINT64_C(0xbaad12345678cafe);
                r[i] = mc100_state_step(s[i], &e, b[i].out, &count[i]);
                assert(count[i] <= 8);
                assert(b[i].before == UINT64_C(0xfeed12345678beef));
                assert(b[i].after == UINT64_C(0xbaad12345678cafe));
            }
            assert(r[0] == r[1] && count[0] == count[1]);
            assert(mc100_state_get(s[0]) == mc100_state_get(s[1]));
            for (i = 0; i < count[0]; ++i) {
                assert(b[0].out[i].id == b[1].out[i].id);
                assert(b[0].out[i].generation == b[1].out[i].generation);
                assert(b[0].out[i].seq == b[1].out[i].seq);
                assert(b[0].out[i].detail == b[1].out[i].detail);
                assert(b[0].out[i].seq_valid == b[1].out[i].seq_valid);
            }
        }
        mc100_state_destroy(s[0]); mc100_state_destroy(s[1]);
    }
}
int main(void)
{
    close_drain_and_retrigger(); armed_required_and_timeouts();
    low_requires_owner_ack(); low_drain_and_pending_release();
    critical_forbids_close_and_fault_scope(); no_data_and_open_deadline();
    monotonic_and_saturation(); low_active_timeout_and_hold_timeout();
    pending_silence_and_global_fault(); fault_retains_both_contexts_until_quiescent();
    deterministic_bounded_replay();
    puts("state: all tests passed");
    return 0;
}
