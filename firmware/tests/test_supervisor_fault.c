#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool ends_with(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool has_suffix(const sup_fake_t *fake, const char *suffix)
{
    for (size_t i = 0; i < mc100_fake_io_count(fake->storage); ++i) {
        const char *path = mc100_fake_io_path(fake->storage, i);
        if (path != NULL && ends_with(path, suffix)) return true;
    }
    return false;
}

static void assert_partial_incident(const sup_fake_t *fake)
{
    const char *partial = NULL;
    for (size_t i = 0; i < mc100_fake_io_count(fake->storage); ++i) {
        const char *path = mc100_fake_io_path(fake->storage, i);
        if (path != NULL && ends_with(path, ".partial.wav")) {
            partial = path;
            break;
        }
    }
    assert(partial != NULL);

    char index[MC100_PATH_BYTES];
    size_t base_len = strlen(partial) - strlen(".partial.wav");
    assert(base_len + strlen(".idx") < sizeof(index));
    memcpy(index, partial, base_len);
    memcpy(index + base_len, ".idx", strlen(".idx") + 1);

    size_t size = 0;
    const uint8_t *bytes = mc100_fake_io_bytes(fake->storage, index, &size);
    assert(bytes != NULL && size >= MC100_INDEX_HEADER_BYTES +
                                  MC100_INDEX_RECORD_BYTES);
    mc100_index_record_t incident;
    assert(mc100_index_decode(bytes + size - MC100_INDEX_RECORD_BYTES,
                              &incident) == MC100_OK);
    assert(incident.type == MC100_INDEX_INCIDENT);
    assert(incident.flags == MC100_INDEX_INCOMPLETE);
    assert(incident.detail != 0);
}

static void admission_full_finalizes_accepted_prefix_as_partial(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);

    bool saw_record = false;
    for (unsigned i = 0; i < 400 && !saw_record; ++i) {
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
        saw_record = mc100_supervisor_state(supervisor) == MC100_RECORD;
    }
    assert(saw_record);

    /* A healthy-storage admission FULL is the one safe-prefix storage fault:
     * the writer may finish the accepted prefix with an INCIDENT record.  Set
     * free space below the admission floor; an I/O callback returning FULL is
     * a storage-I/O failure and must take the fail-closed path instead. */
    mc100_fake_io_free_bytes(fake.storage, 0);
    bool saw_fault = false;
    for (unsigned i = 0; i < 2500; ++i) {
        mc100_result_t result = mc100_supervisor_tick(supervisor);
        assert(result == MC100_OK || result == MC100_FULL || result == MC100_IO);
        if (mc100_supervisor_state(supervisor) == MC100_FAULT) {
            saw_fault = true;
            break;
        }
    }
    assert(saw_fault);
    assert_partial_incident(&fake);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void storage_io_fails_closed_without_metadata_write(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);

    bool saw_record = false;
    for (unsigned i = 0; i < 400 && !saw_record; ++i) {
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
        saw_record = mc100_supervisor_state(supervisor) == MC100_RECORD;
    }
    assert(saw_record);

    /* The next storage operation is the writer's space probe.  An I/O error
     * is not a safe-prefix fault: after it, no INCIDENT/header/truncate/rename
     * write may be attempted and the supervisor must enter FAULT. */
    mc100_fake_io_fault(fake.storage, 1, MC100_IO, false);
    size_t before = mc100_fake_io_log_count(fake.storage);
    bool saw_fault = false;
    for (unsigned i = 0; i < 32; ++i) {
        mc100_result_t result = mc100_supervisor_tick(supervisor);
        assert(result == MC100_IO || result == MC100_NOT_READY ||
               result == MC100_INVALID);
        if (mc100_supervisor_state(supervisor) == MC100_FAULT) {
            saw_fault = true;
            break;
        }
    }
    assert(saw_fault);
    size_t after = mc100_fake_io_log_count(fake.storage);
    for (size_t i = before + 1; i < after; ++i) {
        const mc100_fake_op_t *op = mc100_fake_io_log(fake.storage, i);
        assert(op != NULL);
        assert(op->kind == 'C');
    }
    assert(!has_suffix(&fake, ".partial.wav"));

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    admission_full_finalizes_accepted_prefix_as_partial();
    storage_io_fails_closed_without_metadata_write();
    puts("supervisor_fault: safe-prefix INCIDENT and storage-I/O fail-closed PASS");
    return 0;
}
