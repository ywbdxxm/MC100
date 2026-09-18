#include "mc100_audio.h"
#include <assert.h>
#include <string.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
static unsigned allocation_count;
static bool reject_allocations;
static int audit_allocations(int type, void *data, size_t size, int block,
                            long number, const unsigned char *file, int line)
{
    (void)data; (void)size; (void)block; (void)number; (void)file; (void)line;
    if (type == _HOOK_ALLOC || type == _HOOK_REALLOC) {
        allocation_count++;
        if (reject_allocations) return 0;
    }
    return 1;
}
#endif

static void overflow_preserves_prefix(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_audio_stats_t stats;
    mc100_audio_status_t status;
    mc100_packet_t packet;
    mc100_frame_t frame = {0};
    uint64_t cutoff;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i < 96; ++i) {
        frame.seq = i; frame.pcm[319] = (int16_t)i;
        assert(mc100_audio_push(a, &frame, i == 0) == MC100_OK);
    }
    frame.seq = 96;
    assert(mc100_audio_push(a, &frame, false) == MC100_FULL);
    frame.seq = 97;
    assert(mc100_audio_push(a, &frame, false) == MC100_FULL);
    assert(mc100_audio_stats(a, &stats) == MC100_OK);
    assert(stats.current == 96 && stats.peak == 96 && stats.drops == 1);
    assert(stats.first_gap_valid && stats.first_gap_seq == 96 && stats.first_gap_generation == 1);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.stopped && status.error == MC100_FULL && status.last_accepted_seq == 95);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 95);
    for (uint64_t i = 0; i < 96; ++i) {
        assert(mc100_audio_pop(a, &packet) == MC100_OK);
        assert(packet.generation == 1 && packet.frame.seq == i && packet.frame.pcm[319] == (int16_t)i);
    }
    assert(mc100_audio_pop(a, &packet) == MC100_NOT_READY);
    mc100_audio_destroy(a);
}

/* A full old FIFO must not erase the already frozen new snapshot's cutoff. */
static void trigger_overflow_retains_snapshot_cutoff(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_frame_t frame = {0};
    mc100_audio_status_t status;
    mc100_snapshot_t snapshot;
    uint64_t cutoff;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i < 96; ++i) {
        frame.seq = i;
        assert(mc100_audio_push(a, &frame, i == 0) == MC100_OK);
    }
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 95);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_OK);
    frame.seq = 96;
    assert(mc100_audio_push(a, &frame, false) == MC100_OK);
    assert(mc100_audio_arm(a, 2, 96) == MC100_OK);
    frame.seq = 97;
    assert(mc100_audio_push(a, &frame, true) == MC100_FULL);
    assert(mc100_audio_snapshot(a, 2, &snapshot) == MC100_OK);
    assert(snapshot.first_seq == 96 && snapshot.count == 1);
    assert(mc100_audio_status(a, 2, &status) == MC100_OK);
    assert(status.triggered && status.stopped && status.cutoff_valid);
    assert(status.last_accepted_seq == 96 && status.error == MC100_FULL);
    assert(mc100_audio_stop(a, 2, &cutoff) == MC100_OK && cutoff == 96);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 95);
    mc100_audio_destroy(a);
}

static void cancelling_successor_preserves_wrapped_old_fifo(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_frame_t frame = {0};
    mc100_packet_t packet;
    uint64_t cutoff;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i < 80; ++i) {
        frame.seq = i; frame.pcm[0] = (int16_t)i;
        assert(mc100_audio_push(a, &frame, i == 0) == MC100_OK);
    }
    for (uint64_t i = 0; i < 60; ++i) {
        assert(mc100_audio_pop(a, &packet) == MC100_OK && packet.frame.seq == i);
    }
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 79);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_OK);
    assert(mc100_audio_arm(a, 2, 80) == MC100_OK);
    for (uint64_t i = 80; i < 100; ++i) {
        frame.seq = i;
        assert(mc100_audio_push(a, &frame, i == 80) == MC100_OK);
    }
    assert(mc100_audio_release(a, 2) == MC100_OK);
    for (uint64_t i = 60; i < 80; ++i) {
        assert(mc100_audio_pop(a, &packet) == MC100_OK);
        assert(packet.generation == 1 && packet.frame.seq == i && packet.frame.pcm[0] == (int16_t)i);
    }
    assert(mc100_audio_pop(a, &packet) == MC100_NOT_READY);
    mc100_audio_destroy(a);
}

/* A wrong ring index or per-frame allocator is visible across a million wraps. */
static void million_frames_without_loss_or_hot_allocations(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_audio_stats_t stats;
    mc100_frame_t frame = {0};
    mc100_packet_t packet;
    assert(a);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocation_count = 0;
    _CRT_ALLOC_HOOK previous = _CrtSetAllocHook(audit_allocations);
#endif
    assert(mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i < 1000000; ++i) {
        frame.seq = i;
        for (size_t k = 0; k < 320; ++k) frame.pcm[k] = (int16_t)((i + k) % 32768);
        assert(mc100_audio_push(a, &frame, i == 0) == MC100_OK);
        assert(mc100_audio_pop(a, &packet) == MC100_OK);
        assert(packet.generation == 1 && packet.frame.seq == i);
        assert(memcmp(frame.pcm, packet.frame.pcm, sizeof(frame.pcm)) == 0);
    }
    assert(mc100_audio_stats(a, &stats) == MC100_OK);
    assert(stats.current == 0 && stats.peak == 1 && stats.drops == 0 && !stats.first_gap_valid);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_OK);
    assert(mc100_audio_release(a, 1) == MC100_OK);
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetAllocHook(previous);
    assert(allocation_count == 0);
#endif
    mc100_audio_destroy(a);
}

int main(void)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    reject_allocations = true;
    _CRT_ALLOC_HOOK previous = _CrtSetAllocHook(audit_allocations);
    assert(mc100_audio_create() == NULL);
    _CrtSetAllocHook(previous);
    reject_allocations = false;
#endif
    overflow_preserves_prefix();
    trigger_overflow_retains_snapshot_cutoff();
    cancelling_successor_preserves_wrapped_old_fifo();
    million_frames_without_loss_or_hot_allocations();
    return 0;
}
