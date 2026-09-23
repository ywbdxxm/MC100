#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>

static void enter_record(sup_fake_t *fake, mc100_supervisor_t **out)
{
    sup_fake_set_vad_trigger(fake, 120);
    mc100_supervisor_deps_t deps = sup_fake_deps(fake);
    *out = mc100_supervisor_create(&deps);
    assert(*out != NULL);
    assert(mc100_supervisor_boot(*out) == MC100_OK);
    for (unsigned i = 0; i < 400 &&
         mc100_supervisor_state(*out) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(*out) == MC100_OK);
    assert(mc100_supervisor_state(*out) == MC100_RECORD);
}

static void close_full_enters_fail_closed_cleanup(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *supervisor = NULL;
    enter_record(&fake, &supervisor);

    /* The injected FULL is on the first close() after recording starts.  A
     * close failure is not an admission-full safe prefix: the final marker
     * may be missing, so the owner must abandon and release handles rather
     * than leave the writer live while State waits in FAULT. */
    fake.fail_close_result = MC100_FULL;
    fake.fail_close_once = true;
    bool left_record = false;
    for (unsigned i = 0; i < 1000; ++i) {
        mc100_result_t r = mc100_supervisor_tick(supervisor);
        assert(r == MC100_OK || r == MC100_FULL || r == MC100_IO ||
               r == MC100_NOT_READY);
        if (mc100_supervisor_state(supervisor) != MC100_RECORD) {
            left_record = true;
            break;
        }
    }
    assert(left_record);

    mc100_writer_status_t status;
    assert(mc100_supervisor_writer_status(supervisor, &status) == MC100_OK);
    assert(status.abandoned);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void hold_cleanup_retries_after_release_failure(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);
    for (unsigned i = 0; i < 400 &&
         mc100_supervisor_state(supervisor) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_RECORD);

    /* HOLD must not acknowledge quiescence when its no-write handle cleanup
     * fails.  The next owner tick retries the same cleanup transaction. */
    fake.fail_close_once = true;
    fake.fail_close_result = MC100_IO;
    mc100_event_t critical = {.id = MC100_EV_CRITICAL, .now_ms = fake.now,
                              .global = true};
    assert(mc100_supervisor_post_event(supervisor, &critical) == MC100_OK);
    assert(mc100_supervisor_drain_events(supervisor) == MC100_IO);
    assert(mc100_supervisor_state(supervisor) != MC100_LOW_BAT_HOLD);
    assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_LOW_BAT_HOLD);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    close_full_enters_fail_closed_cleanup();
    hold_cleanup_retries_after_release_failure();
    puts("supervisor_edges: close FULL fail-closed cleanup PASS");
    return 0;
}
