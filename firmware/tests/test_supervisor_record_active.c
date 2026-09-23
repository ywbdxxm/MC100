#include "mc100_supervisor.h"
#include "supervisor_test_support.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t sync_count_since(const sup_fake_t *fake, size_t start)
{
    size_t count = 0;
    for (size_t i = start; i < mc100_fake_io_log_count(fake->storage); ++i)
        if (mc100_fake_io_log(fake->storage, i)->kind == 'S')
            ++count;
    return count;
}

static bool has_segment_one_part(const sup_fake_t *fake)
{
    for (size_t i = 0; i < mc100_fake_io_count(fake->storage); ++i) {
        const char *path = mc100_fake_io_path(fake->storage, i);
        if (path != NULL && strstr(path, "_1.wav.part") != NULL)
            return true;
    }
    return false;
}

static void enter_record(sup_fake_t *fake, mc100_supervisor_t **out)
{
    sup_fake_set_vad_trigger(fake, 120);
    mc100_supervisor_deps_t deps = sup_fake_deps(fake);
    *out = mc100_supervisor_create(&deps);
    assert(*out != NULL);
    assert(mc100_supervisor_boot(*out) == MC100_OK);
    for (unsigned i = 0; i < 300 &&
         mc100_supervisor_state(*out) != MC100_RECORD; ++i)
        assert(mc100_supervisor_tick(*out) == MC100_OK);
    assert(mc100_supervisor_state(*out) == MC100_RECORD);
}

static void record_ticks_append_every_live_frame_and_checkpoint(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *supervisor = NULL;
    enter_record(&fake, &supervisor);

    size_t log_start = mc100_fake_io_log_count(fake.storage);
    for (unsigned i = 0; i < 200; ++i) {
        fake.now = UINT64_C(1000) + (uint64_t)i * 10;
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    }

    /* 100-frame pre-roll plus all 200 live frames must be appended. */
    uint64_t written = sup_fake_written_frames(&fake);
    size_t checkpoints = sync_count_since(&fake, log_start);
    fprintf(stderr, "record-active RED: written=%llu expected=%u checkpoints=%zu\n",
            (unsigned long long)written,
            (unsigned)(MC100_PREROLL_FRAMES + 200), checkpoints);
    assert(written == MC100_PREROLL_FRAMES + 200);
    assert(checkpoints > 0);

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

static void record_rotation_starts_segment_one_at_boundary(void)
{
    sup_fake_t fake;
    sup_fake_init(&fake);
    mc100_supervisor_t *supervisor = NULL;
    enter_record(&fake, &supervisor);

    /* The trigger frame is queued when RECORD opens.  The first RECORD tick
     * appends it, so this tick count reaches exactly 15001 frames total. */
    for (unsigned i = 0; i < MC100_SEGMENT_FRAMES - MC100_PREROLL_FRAMES + 1;
         ++i) {
        fake.now = UINT64_C(1000) + (uint64_t)i;
        assert(mc100_supervisor_tick(supervisor) == MC100_OK);
    }

    assert(has_segment_one_part(&fake));

    mc100_supervisor_destroy(supervisor);
    sup_fake_destroy(&fake);
}

int main(void)
{
    record_ticks_append_every_live_frame_and_checkpoint();
    record_rotation_starts_segment_one_at_boundary();
    return 0;
}
