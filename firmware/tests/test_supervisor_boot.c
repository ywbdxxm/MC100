#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>

static void boot_reaches_listen_after_successful_recovery(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_OK);
    assert(fake.recovery_called == 1);
    assert(mc100_supervisor_state(supervisor) == MC100_LISTEN);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void recovery_timeout_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    fake.advance_on_list_ms = 30000;
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_TIMEOUT);
    assert(fake.recovery_called == 1);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void recovery_io_error_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_fake_io_fault(fake.storage, 1, MC100_IO, false);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_IO);
    assert(fake.recovery_called == 1);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    boot_reaches_listen_after_successful_recovery();
    recovery_timeout_never_enters_listen();
    recovery_io_error_never_enters_listen();
    return 0;
}
