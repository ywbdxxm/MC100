#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>

static void trigger_opens_record_with_preroll(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);

    for (unsigned i = 0; i < 300 &&
         mc100_supervisor_state(supervisor) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);

    assert(mc100_supervisor_state(supervisor) == MC100_RECORD);
    mc100_writer_status_t status;
    assert(mc100_supervisor_writer_status(supervisor, &status) == MC100_OK);
    assert(status.active);
    assert(status.segment_index == 0);
    assert(status.accepted_bytes == MC100_PREROLL_FRAMES * 640u);
    sup_fake_assert_preroll(&fake, &status, 20, MC100_PREROLL_FRAMES);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    trigger_opens_record_with_preroll();
    return 0;
}
