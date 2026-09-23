#include "mc100_supervisor.h"

#include <stdlib.h>

struct mc100_supervisor {
    mc100_supervisor_deps_t deps;
    mc100_state_t *state;
    mc100_audio_t *audio;
    mc100_writer_t *writer;
    bool booted;
};

static mc100_result_t supervisor_fault(mc100_supervisor_t *supervisor,
                                        mc100_result_t cause)
{
    mc100_event_t event = {
        .id = MC100_EV_FAULT,
        .generation = 0,
        .now_ms = supervisor->deps.now_ms(supervisor->deps.clock_ctx),
        .detail = MC100_FAULT_RECOVERY_REQUIRED,
        .global = true,
    };
    mc100_action_t actions[MC100_STATE_ACTION_CAPACITY];
    size_t count = 0;
    mc100_result_t result = mc100_state_step(supervisor->state, &event,
                                              actions, &count);
    return result == MC100_OK ? cause : result;
}

static mc100_result_t supervisor_boot_failure(mc100_supervisor_t *supervisor,
                                               mc100_result_t cause)
{
    mc100_writer_abandon(supervisor->writer,
                         MC100_FAULT_RECOVERY_REQUIRED);
    (void)mc100_writer_release_handles(supervisor->writer);
    return supervisor_fault(supervisor, cause);
}

static mc100_result_t supervisor_pump_event(mc100_supervisor_t *supervisor,
                                             const mc100_event_t *event)
{
    mc100_action_t actions[MC100_STATE_ACTION_CAPACITY];
    size_t count = 0;
    mc100_result_t result = mc100_state_step(supervisor->state, event, actions,
                                              &count);
    if (result != MC100_OK)
        return result;

    for (size_t i = 0; i < count; ++i) {
        if (actions[i].id != MC100_ACT_ARM)
            continue;
        result = mc100_audio_arm(supervisor->audio, actions[i].generation,
                                 actions[i].seq_valid ? actions[i].seq : 0);
        if (result != MC100_OK)
            return supervisor_fault(supervisor, result);

        mc100_event_t armed = {
            .id = MC100_EV_ARMED,
            .generation = actions[i].generation,
            .now_ms = event->now_ms,
        };
        result = supervisor_pump_event(supervisor, &armed);
        if (result != MC100_OK)
            return result;
    }
    return MC100_OK;
}

mc100_supervisor_t *mc100_supervisor_create(
    const mc100_supervisor_deps_t *deps)
{
    if (deps == NULL || deps->io == NULL || deps->now_ms == NULL ||
        deps->boot_id == NULL)
        return NULL;

    mc100_supervisor_t *supervisor = calloc(1, sizeof(*supervisor));
    if (supervisor == NULL)
        return NULL;
    supervisor->deps = *deps;
    supervisor->state = mc100_state_create();
    supervisor->audio = mc100_audio_create();
    supervisor->writer = mc100_writer_create(deps->io, deps->io_ctx,
                                             deps->boot_id);
    if (supervisor->state == NULL || supervisor->audio == NULL ||
        supervisor->writer == NULL) {
        mc100_writer_destroy(supervisor->writer);
        mc100_audio_destroy(supervisor->audio);
        mc100_state_destroy(supervisor->state);
        free(supervisor);
        return NULL;
    }
    return supervisor;
}

void mc100_supervisor_destroy(mc100_supervisor_t *supervisor)
{
    if (supervisor == NULL)
        return;
    mc100_writer_destroy(supervisor->writer);
    mc100_audio_destroy(supervisor->audio);
    mc100_state_destroy(supervisor->state);
    free(supervisor);
}

mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *supervisor)
{
    if (supervisor == NULL || supervisor->booted ||
        mc100_state_get(supervisor->state) != MC100_BOOT)
        return MC100_INVALID;

    uint64_t deadline = supervisor->deps.now_ms(supervisor->deps.clock_ctx) +
                        UINT64_C(30000);
    mc100_recovery_report_t report;
    mc100_result_t result = mc100_recover(
        supervisor->deps.io, supervisor->deps.io_ctx, deadline,
        supervisor->deps.now_ms, supervisor->deps.clock_ctx, &report);
    if (result != MC100_OK)
        return supervisor_boot_failure(supervisor, result);

    result = mc100_writer_prepare(supervisor->writer);
    if (result != MC100_OK)
        return supervisor_boot_failure(supervisor, result);
    mc100_writer_status_t writer_status;
    result = mc100_writer_status(supervisor->writer, &writer_status);
    if (result != MC100_OK || writer_status.prepared_slots < 2)
        return supervisor_boot_failure(supervisor,
                                       result == MC100_OK ? MC100_NOT_READY
                                                          : result);

    if (supervisor->deps.battery_ready == NULL ||
        !supervisor->deps.battery_ready(supervisor->deps.battery_ctx))
        return supervisor_boot_failure(supervisor, MC100_NOT_READY);
    if (supervisor->deps.driver_ready == NULL ||
        !supervisor->deps.driver_ready(supervisor->deps.driver_ctx))
        return supervisor_boot_failure(supervisor, MC100_NOT_READY);

    mc100_event_t ready = {
        .id = MC100_EV_READY,
        .now_ms = supervisor->deps.now_ms(supervisor->deps.clock_ctx),
        .detail = 2,
        .global = true,
    };
    result = supervisor_pump_event(supervisor, &ready);
    if (result == MC100_OK)
        supervisor->booted = true;
    else
        result = supervisor_boot_failure(supervisor, result);
    return result;
}

mc100_result_t mc100_supervisor_tick(mc100_supervisor_t *supervisor)
{
    if (supervisor == NULL || !supervisor->booted)
        return MC100_NOT_READY;
    return MC100_OK;
}

mc100_state_id_t mc100_supervisor_state(
    const mc100_supervisor_t *supervisor)
{
    return supervisor == NULL ? MC100_FAULT
                               : mc100_state_get(supervisor->state);
}
