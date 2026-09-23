#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned calls;
    char name[MC100_PATH_BYTES];
} upload_spy_t;

static void upload_closed(void *ctx, const char *name)
{
    upload_spy_t *spy = ctx;
    ++spy->calls;
    if (name != NULL)
        (void)snprintf(spy->name, sizeof(spy->name), "%s", name);
}

static bool ends_with(const char *path, const char *suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len &&
           strcmp(path + path_len - suffix_len, suffix) == 0;
}

static unsigned finalized_wavs(const sup_fake_t *fake)
{
    unsigned count = 0;
    for (size_t i = 0; i < mc100_fake_io_count(fake->storage); ++i) {
        const char *path = mc100_fake_io_path(fake->storage, i);
        if (path != NULL && ends_with(path, ".wav"))
            ++count;
    }
    return count;
}

static void silence_closes_once_and_rearms(void)
{
    sup_fake_t fake;
    upload_spy_t upload = {0};
    sup_fake_init(&fake);
    sup_fake_set_vad_trigger(&fake, 120);

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    deps.upload = (mc100_upload_sink_t){upload_closed, &upload};
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);

    bool saw_record = false;
    for (unsigned i = 0; i < 2500; ++i) {
        mc100_result_t result = mc100_supervisor_tick(supervisor);
        assert(result == MC100_OK);
        if (mc100_supervisor_state(supervisor) == MC100_RECORD)
            saw_record = true;
        if (saw_record && mc100_supervisor_state(supervisor) == MC100_LISTEN &&
            upload.calls == 1)
            break;
    }

    /* 750 post-trigger silent frames must drive STOP -> CLOSE -> RELEASE. */
    assert(upload.calls == 1);
    assert(ends_with(upload.name, ".wav"));
    assert(!ends_with(upload.name, ".partial.wav"));
    assert(finalized_wavs(&fake) == 1);
    assert(mc100_supervisor_state(supervisor) == MC100_LISTEN);

    /* A close is terminal for this generation; further LISTEN ticks must not
     * repeat the upload notification.  The implementation also has to arm the
     * next generation so the state remains usable. */
    for (unsigned i = 0; i < 8; ++i)
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    assert(mc100_supervisor_state(supervisor) == MC100_LISTEN);
    assert(upload.calls == 1);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    silence_closes_once_and_rearms();
    puts("supervisor_close: silence close, upload exactly once, re-arm PASS");
    return 0;
}
