#include "mc100_battery_policy.h"
#include <stdlib.h>
struct mc100_battery_policy {
    mc100_battery_config_t config;
    mc100_battery_result_t latched;
    uint64_t now, low_since, resume_since;
    bool have_time, stable, low_running, resume_running;
};
mc100_battery_policy_t *mc100_battery_create(const mc100_battery_config_t *c)
{
    mc100_battery_policy_t *p;
    if (!c || c->critical_mv == 0 || c->critical_mv >= c->low_mv ||
        c->low_mv >= c->resume_mv || c->low_duration_ms == 0 ||
        c->resume_duration_ms == 0) return NULL;
    p = calloc(1, sizeof(*p));
    if (p) p->config = *c;
    return p;
}
void mc100_battery_destroy(mc100_battery_policy_t *p) { free(p); }
bool mc100_battery_is_stable(const mc100_battery_policy_t *p)
{ return p && p->stable; }
mc100_battery_result_t mc100_battery_step(mc100_battery_policy_t *p,
    uint32_t mv, bool valid, uint64_t now)
{
    if (!p || (p->have_time && now < p->now)) return MC100_BATTERY_INVALID;
    p->have_time = true;
    p->now = now;
    if (!valid) {
        p->stable = false;
        p->low_running = false;
        p->resume_running = false;
        return MC100_BATTERY_INVALID;
    }
    if (mv <= p->config.critical_mv) {
        p->latched = MC100_BATTERY_CRITICAL;
        p->stable = true;
        p->resume_running = false;
        return p->latched;
    }
    if (p->latched != MC100_BATTERY_NORMAL) {
        p->stable = true;
        if (mv < p->config.resume_mv) p->resume_running = false;
        else {
            if (!p->resume_running) {
                p->resume_running = true;
                p->resume_since = now;
            }
            if (now - p->resume_since >= p->config.resume_duration_ms) {
                p->latched = MC100_BATTERY_NORMAL;
                p->resume_running = false;
                p->low_running = false;
                return MC100_BATTERY_RECOVERED;
            }
        }
        return p->latched;
    }
    if (mv > p->config.low_mv) {
        p->stable = true;
        p->low_running = false;
    } else {
        if (!p->low_running) {
            p->low_running = true;
            p->low_since = now;
        }
        if (now - p->low_since >= p->config.low_duration_ms) {
            p->latched = MC100_BATTERY_LOW;
            p->stable = true;
        }
    }
    return p->latched;
}
