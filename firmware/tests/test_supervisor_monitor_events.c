#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>

static mc100_supervisor_t *booted(sup_fake_t *fake)
{
    mc100_supervisor_deps_t deps = sup_fake_deps(fake);
    mc100_supervisor_t *s = mc100_supervisor_create(&deps);
    assert(s != NULL);
    assert(mc100_supervisor_boot(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_LISTEN);
    return s;
}

static mc100_event_t monitor_event(mc100_event_id_t id, uint64_t at)
{
    mc100_event_t e = {.id = id, .now_ms = at, .global = true};
    return e;
}

static unsigned lock_calls;
static unsigned unlock_calls;
static void monitor_lock(void *ctx)
{
    (void)ctx;
    ++lock_calls;
}
static void monitor_unlock(void *ctx)
{
    (void)ctx;
    ++unlock_calls;
}

static void low_and_critical_are_owner_drained(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *s = booted(&fake);

    mc100_event_t low = monitor_event(MC100_EV_LOW, 1);
    assert(mc100_supervisor_post_event(s, &low) == MC100_OK);
    mc100_event_t owner_only = monitor_event(MC100_EV_OPENED, 1);
    assert(mc100_supervisor_post_event(s, &owner_only) == MC100_INVALID);
    /* Posting does not mutate State or touch Audio from the monitor side. */
    assert(mc100_supervisor_state(s) == MC100_LISTEN);
    assert(mc100_supervisor_drain_events(s) == MC100_OK);
    /* LOW revokes the pending ARM token; the owner acknowledges STOP and
     * quiesces it in the same bounded action batch. */
    assert(mc100_supervisor_state(s) == MC100_LOW_BAT_HOLD);

    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);

    /* A fresh LISTEN owner demonstrates that CRITICAL itself reaches the
     * no-write hold path, independent of LOW's already-quiesced state. */
    sup_fake_init(&fake);
    s = booted(&fake);

    mc100_event_t critical = monitor_event(MC100_EV_CRITICAL, 2);
    assert(mc100_supervisor_post_event(s, &critical) == MC100_OK);
    assert(mc100_supervisor_drain_events(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_LOW_BAT_HOLD);

    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);
}

static void critical_survives_a_full_normal_queue(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    deps.event_lock = monitor_lock;
    deps.event_unlock = monitor_unlock;
    deps.event_lock_ctx = NULL;
    mc100_supervisor_t *s = mc100_supervisor_create(&deps);
    assert(s != NULL);
    assert(mc100_supervisor_boot(s) == MC100_OK);

    mc100_event_t tick = monitor_event(MC100_EV_TICK, 100);
    for (size_t i = 0; i < MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY; ++i)
        assert(mc100_supervisor_post_event(s, &tick) == MC100_OK);
    assert(mc100_supervisor_post_event(s, &tick) == MC100_FULL);

    mc100_event_t critical = monitor_event(MC100_EV_CRITICAL, 100);
    assert(mc100_supervisor_post_event(s, &critical) == MC100_OK);
    assert(mc100_supervisor_drain_events(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_LOW_BAT_HOLD);
    assert(lock_calls != 0 && unlock_calls == lock_calls);

    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);
}

static void diagnostic_events_use_the_same_seam(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *s = booted(&fake);

    mc100_event_t tick = monitor_event(MC100_EV_TICK, 1);
    assert(mc100_supervisor_post_event(s, &tick) == MC100_OK);
    mc100_event_t adc = monitor_event(MC100_EV_FAULT, 2);
    adc.detail = MC100_FAULT_ADC_INVALID;
    assert(mc100_supervisor_post_event(s, &adc) == MC100_OK);
    /* Card removal is represented by the non-droppable CRITICAL intent. */
    mc100_event_t card = monitor_event(MC100_EV_CRITICAL, 3);
    assert(mc100_supervisor_post_event(s, &card) == MC100_OK);
    assert(mc100_supervisor_drain_events(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_LOW_BAT_HOLD ||
           mc100_supervisor_state(s) == MC100_FAULT);

    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);
}

int main(void)
{
    low_and_critical_are_owner_drained();
    critical_survives_a_full_normal_queue();
    diagnostic_events_use_the_same_seam();
    puts("supervisor_monitor_events: post/drain seam PASS");
    return 0;
}
