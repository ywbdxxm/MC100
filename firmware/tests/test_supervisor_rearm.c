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

static void event(mc100_supervisor_t *s, mc100_event_id_t id, uint64_t at)
{
    mc100_event_t e = {.id = id, .now_ms = at, .global = true};
    assert(mc100_supervisor_post_event(s, &e) == MC100_OK);
    assert(mc100_supervisor_drain_events(s) == MC100_OK);
}

static void rearm_after_power_hold_recreates_owner_state(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *s = booted(&fake);

    event(s, MC100_EV_CRITICAL, 1);
    assert(mc100_supervisor_state(s) == MC100_LOW_BAT_HOLD);
    event(s, MC100_EV_RECOVERED_POWER, 2);
    assert(mc100_supervisor_state(s) == MC100_BOOT);

    /* The platform owner remounts storage before this call.  Re-arm must
     * discard terminal Audio/Writer instances and allow the next tick to run
     * the full recovery -> prepare -> driver-ready boot gate again. */
    assert(mc100_supervisor_rearm(s) == MC100_OK);
    assert(mc100_supervisor_tick(s) == MC100_OK);
    assert(mc100_supervisor_state(s) == MC100_LISTEN);

    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);
}

static void rearm_is_rejected_until_power_event(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *s = booted(&fake);
    assert(mc100_supervisor_rearm(s) == MC100_INVALID);
    mc100_supervisor_destroy(s);
    sup_fake_destroy(&fake);
}

int main(void)
{
    rearm_after_power_hold_recreates_owner_state();
    rearm_is_rejected_until_power_event();
    puts("supervisor_rearm: lifecycle PASS");
    return 0;
}
