#include "mc100_battery_policy.h"
#include <assert.h>
#include <stdio.h>

static const mc100_battery_config_t evt = {3600, 3450, 3800, 3000, 30000};
static void startup_low_and_recovery(void)
{
    mc100_battery_policy_t *p = mc100_battery_create(&evt);
    assert(p && !mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, 0) == MC100_BATTERY_NORMAL);
    assert(!mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, 2999) == MC100_BATTERY_NORMAL);
    assert(!mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, 3000) == MC100_BATTERY_LOW);
    assert(mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3800, true, 3001) == MC100_BATTERY_LOW);
    assert(mc100_battery_step(p, 3800, true, 33000) == MC100_BATTERY_LOW);
    assert(mc100_battery_step(p, 3800, true, 33001) == MC100_BATTERY_RECOVERED);
    assert(mc100_battery_step(p, 3800, true, 33001) == MC100_BATTERY_NORMAL);
    mc100_battery_destroy(p);
}
static void glitches_invalid_and_critical(void)
{
    mc100_battery_policy_t *p = mc100_battery_create(&evt);
    assert(mc100_battery_step(p, 3800, true, 0) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, 1) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_step(p, 3601, true, 3000) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_step(p, 3600, true, 3001) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_step(p, 3600, false, 6001) == MC100_BATTERY_INVALID);
    assert(!mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, 6002) == MC100_BATTERY_NORMAL);
    assert(!mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3450, true, 6003) == MC100_BATTERY_CRITICAL);
    assert(mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3700, true, 6004) == MC100_BATTERY_CRITICAL);
    assert(mc100_battery_step(p, 3800, true, 6005) == MC100_BATTERY_CRITICAL);
    assert(mc100_battery_step(p, 3799, true, 36005) == MC100_BATTERY_CRITICAL);
    assert(mc100_battery_step(p, 3800, true, 36006) == MC100_BATTERY_CRITICAL);
    assert(mc100_battery_step(p, 3800, true, 66006) == MC100_BATTERY_RECOVERED);
    mc100_battery_destroy(p);
}
static void boundaries_and_bad_configuration(void)
{
    mc100_battery_config_t c = evt;
    mc100_battery_policy_t *p;
    assert(mc100_battery_create(NULL) == NULL);
    c.resume_mv = c.low_mv;
    assert(mc100_battery_create(&c) == NULL);
    c = evt; c.critical_mv = c.low_mv;
    assert(mc100_battery_create(&c) == NULL);
    c = evt; c.low_duration_ms = 0;
    assert(mc100_battery_create(&c) == NULL);
    c = evt; c.resume_duration_ms = 0;
    assert(mc100_battery_create(&c) == NULL);
    assert(mc100_battery_step(NULL, 3800, true, 0) == MC100_BATTERY_INVALID);
    assert(!mc100_battery_is_stable(NULL));
    p = mc100_battery_create(&evt);
    assert(mc100_battery_step(p, 3600, true, UINT64_MAX - 3000) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_step(p, 3900, true, UINT64_MAX - 3001) == MC100_BATTERY_INVALID);
    assert(!mc100_battery_is_stable(p));
    assert(mc100_battery_step(p, 3600, true, UINT64_MAX - 1) == MC100_BATTERY_NORMAL);
    assert(mc100_battery_step(p, 3600, true, UINT64_MAX) == MC100_BATTERY_LOW);
    assert(mc100_battery_step(p, 3600, true, UINT64_MAX) == MC100_BATTERY_LOW);
    mc100_battery_destroy(p);
}
int main(void)
{
    startup_low_and_recovery(); glitches_invalid_and_critical();
    boundaries_and_bad_configuration();
    puts("battery_policy: all tests passed");
    return 0;
}
