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
        if (path != NULL && strstr(path + 33, "reserve_") == NULL &&
            ends_with(path, suffix))
            return true;
    }
    return false;
}

static void interrupted_record_is_recovered_on_next_boot(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    mc100_supervisor_t *first = mc100_supervisor_create(&deps);
    assert(first != NULL);
    assert(mc100_supervisor_boot(first) == MC100_OK);

    bool saw_record = false;
    for (unsigned i = 0; i < 400 && !saw_record; ++i) {
        assert(mc100_supervisor_tick(first) == MC100_OK);
        saw_record = mc100_supervisor_state(first) == MC100_RECORD;
    }
    assert(saw_record);

    /* Leave a committed checkpoint and then simulate an abrupt power loss by
     * destroying the supervisor without STOP/CLOSE. */
    for (unsigned i = 0; i < 50; ++i)
        assert(mc100_supervisor_tick(first) == MC100_OK);
    fake.now = 1000;
    assert(mc100_supervisor_tick(first) == MC100_OK);
    assert(has_suffix(&fake, ".wav.part"));
    mc100_supervisor_destroy(first);

    mc100_supervisor_t *second = mc100_supervisor_create(&deps);
    assert(second != NULL);
    assert(mc100_supervisor_boot(second) == MC100_OK);

    /* Recovery creates a sibling recovered WAV and never destroys the source
     * part file, preserving evidence for a later forensic pass. */
    assert(has_suffix(&fake, ".recovered.wav"));
    assert(has_suffix(&fake, ".wav.part"));
    assert(mc100_supervisor_state(second) == MC100_LISTEN);

    mc100_supervisor_destroy(second);
    sup_fake_destroy(&fake);
}

int main(void)
{
    interrupted_record_is_recovered_on_next_boot();
    puts("supervisor_recover_roundtrip: interrupted prefix recovered, source kept PASS");
    return 0;
}
