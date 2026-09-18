#ifndef MC100_BATTERY_POLICY_H
#define MC100_BATTERY_POLICY_H

#include "mc100_types.h"

typedef struct mc100_battery_policy mc100_battery_policy_t;
typedef struct {
    uint32_t low_mv, critical_mv, resume_mv;
    uint64_t low_duration_ms, resume_duration_ms;
} mc100_battery_config_t;
typedef enum { MC100_BATTERY_NORMAL, MC100_BATTERY_LOW,
    MC100_BATTERY_CRITICAL, MC100_BATTERY_RECOVERED,
    MC100_BATTERY_INVALID } mc100_battery_result_t;

/* No production voltage defaults. EVT simulation uses 3600/3450/3800 mV,
 * 3000/30000 ms; physical qualification must select release configuration.
 * is_stable is false at startup, on invalid ADC, and while the initial low
 * qualification is unresolved. NORMAL alone never authorizes BOOT writes.
 * Initial mv > low resolves immediately; mv <= low qualifies for low_duration.
 * ADC-invalid samples interrupt all qualification runs but do not clear LOW.
 * Backward time returns INVALID with no state mutation (including stability).
 * RECOVERED is a one-sample notification after continuous resume qualification;
 * the next normal sample returns NORMAL. LOW/CRITICAL remain latched until then.
 */
mc100_battery_policy_t *mc100_battery_create(const mc100_battery_config_t *config);
void mc100_battery_destroy(mc100_battery_policy_t *policy);
mc100_battery_result_t mc100_battery_step(mc100_battery_policy_t *policy,
    uint32_t mv, bool valid, uint64_t now_ms);
bool mc100_battery_is_stable(const mc100_battery_policy_t *policy);

#endif
