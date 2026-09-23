#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>

typedef struct {
    unsigned calls;
    mc100_supervisor_audio_control_t commands[8];
    bool fail_stop;
} audio_owner_spy_t;

static mc100_result_t audio_owner_control(
    void *context, mc100_supervisor_audio_control_t command,
    mc100_generation_t generation)
{
    audio_owner_spy_t *spy = context;
    assert(generation != 0);
    assert(spy->calls < sizeof(spy->commands) / sizeof(spy->commands[0]));
    spy->commands[spy->calls++] = command;
    if (command == MC100_SUPERVISOR_AUDIO_STOP && spy->fail_stop) {
        spy->fail_stop = false;
        return MC100_IO;
    }
    return MC100_OK;
}

static mc100_supervisor_t *recording(sup_fake_t *fake,
                                     audio_owner_spy_t *spy)
{
    sup_fake_set_vad_trigger(fake, 120);
    mc100_supervisor_deps_t deps = sup_fake_deps(fake);
    deps.audio_control = audio_owner_control;
    deps.audio_control_ctx = spy;
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);
    for (unsigned i = 0; i < 400 &&
         mc100_supervisor_state(supervisor) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_RECORD);
    assert(spy->calls >= 1 &&
           spy->commands[0] == MC100_SUPERVISOR_AUDIO_START);
    return supervisor;
}

static void stop_failure_suppresses_state_ack_and_retries(void)
{
    sup_fake_t fake;
    audio_owner_spy_t spy = {0};
    sup_fake_init(&fake);
    mc100_supervisor_t *supervisor = recording(&fake, &spy);

    spy.fail_stop = true;
    mc100_event_t critical = {.id = MC100_EV_CRITICAL, .now_ms = fake.now,
                              .global = true};
    assert(mc100_supervisor_post_event(supervisor, &critical) == MC100_OK);
    assert(mc100_supervisor_drain_events(supervisor) == MC100_IO);
    assert(mc100_supervisor_state(supervisor) != MC100_LOW_BAT_HOLD);

    assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_LOW_BAT_HOLD);
    assert(spy.calls >= 3);
    assert(spy.commands[spy.calls - 1] == MC100_SUPERVISOR_AUDIO_STOP);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void successful_stop_precedes_held_ack(void)
{
    sup_fake_t fake;
    audio_owner_spy_t spy = {0};
    sup_fake_init(&fake);
    mc100_supervisor_t *supervisor = recording(&fake, &spy);

    mc100_event_t critical = {.id = MC100_EV_CRITICAL, .now_ms = fake.now,
                              .global = true};
    assert(mc100_supervisor_post_event(supervisor, &critical) == MC100_OK);
    assert(mc100_supervisor_drain_events(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_LOW_BAT_HOLD);
    assert(spy.commands[spy.calls - 1] == MC100_SUPERVISOR_AUDIO_STOP);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    stop_failure_suppresses_state_ack_and_retries();
    successful_stop_precedes_held_ack();
    puts("supervisor_audio_owner: owner START/STOP ACK ordering PASS");
    return 0;
}
