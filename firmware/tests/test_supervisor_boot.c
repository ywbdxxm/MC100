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
    assert(fake.battery_calls == 1);
    assert(fake.driver_calls == 1);
    assert(fake.readiness_order_ok);
    assert(mc100_supervisor_state(supervisor) == MC100_LISTEN);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void prepare_failure_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_fake_io_fault(fake.storage, 2, MC100_IO, false);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_IO);
    assert(fake.battery_calls == 1);
    assert(fake.driver_calls == 0);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void battery_failure_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    fake.battery_is_ready = false;
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_NOT_READY);
    assert(fake.recovery_called == 0);
    assert(mc100_fake_io_log_count(fake.storage) == 0);
    assert(fake.battery_calls == 1);
    assert(fake.driver_calls == 0);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void partial_prepare_close_failure_is_returned_as_fault(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_fake_io_fault(fake.storage, 8, MC100_FULL, false);
    fake.fail_close_once = true;
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_IO);
    assert(fake.close_calls >= 2);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void near_maximum_clock_does_not_wrap_recovery_deadline(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    fake.now = UINT64_MAX - 1;
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_LISTEN);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void driver_failure_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    fake.driver_is_ready = false;
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_NOT_READY);
    assert(fake.battery_calls == 1);
    assert(fake.driver_calls == 1);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void missing_battery_gate_never_enters_listen(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    deps.battery_ready = NULL;
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);

    assert(mc100_supervisor_boot(supervisor) == MC100_NOT_READY);
    assert(fake.driver_calls == 0);
    assert(mc100_supervisor_state(supervisor) == MC100_FAULT);

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
    prepare_failure_never_enters_listen();
    battery_failure_never_enters_listen();
    driver_failure_never_enters_listen();
    missing_battery_gate_never_enters_listen();
    partial_prepare_close_failure_is_returned_as_fault();
    near_maximum_clock_does_not_wrap_recovery_deadline();
    recovery_timeout_never_enters_listen();
    recovery_io_error_never_enters_listen();
    return 0;
}
