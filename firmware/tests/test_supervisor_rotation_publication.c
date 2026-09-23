#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned calls;
    char names[MC100_WRITER_PUBLICATION_CAPACITY][MC100_PATH_BYTES];
} upload_spy_t;

static void upload_closed(void *ctx, const char *name)
{
    upload_spy_t *spy = ctx;
    assert(spy->calls < MC100_WRITER_PUBLICATION_CAPACITY);
    (void)snprintf(spy->names[spy->calls], MC100_PATH_BYTES, "%s", name);
    ++spy->calls;
}

static bool continuous_speech(void *ctx, const mc100_frame_t *frame)
{
    (void)ctx;
    (void)frame;
    return true;
}

int main(void)
{
    sup_fake_t fake;
    upload_spy_t upload = {0};
    sup_fake_init(&fake);
    fake.generate_pcm = true;

    mc100_supervisor_deps_t deps = sup_fake_deps(&fake);
    deps.vad = (mc100_vad_t){continuous_speech, NULL};
    deps.upload = (mc100_upload_sink_t){upload_closed, &upload};
    mc100_supervisor_t *supervisor = mc100_supervisor_create(&deps);
    assert(supervisor != NULL);
    assert(mc100_supervisor_boot(supervisor) == MC100_OK);

    /* Continuous speech keeps RECORD alive long enough for the writer-owned
     * 15,000-frame rotation.  The supervisor must consume the publication
     * emitted by writer_append and forward the exact writer-owned name. */
    for (unsigned i = 0; i < MC100_SEGMENT_FRAMES + 200; ++i) {
        fake.now = UINT64_C(1000) + i;
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    }

    assert(upload.calls == 1);
    assert(strcmp(upload.names[0],
                  "000102030405060708090a0b0c0d0e0f_1_0.wav") == 0);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
    puts("supervisor_rotation_publication: writer-owned rotation notification PASS");
    return 0;
}
