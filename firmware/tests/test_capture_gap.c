#include "mc100_audio.h"
#include <assert.h>

static mc100_result_t count_frame(void *context, const mc100_frame_t *frame)
{
    unsigned *count = context;
    (void)frame;
    ++*count;
    return MC100_OK;
}
static void assembler_gap_never_fills_missing_audio(void)
{
    mc100_assembler_t assembler;
    uint8_t pcm[640] = {0};
    unsigned count = 0;
    mc100_assembler_init(&assembler, 0);
    assert(mc100_assembler_feed(&assembler, pcm, 639, count_frame, &count) == MC100_OK);
    assert(count == 0);
    assert(mc100_assembler_capture_gap(&assembler) == MC100_IO);
    assert(mc100_assembler_feed(&assembler, pcm, 640, count_frame, &count) == MC100_IO);
    assert(count == 0);
    mc100_assembler_init(&assembler, 5);
    assert(mc100_assembler_feed(&assembler, pcm, 640, count_frame, &count) == MC100_OK);
    assert(count == 1);
}
static void discontinuity_preserves_only_valid_prefix(uint64_t bad_seq)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_frame_t frame = {0};
    mc100_audio_status_t status;
    mc100_audio_stats_t stats;
    mc100_packet_t packet;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    frame.seq = 10;
    assert(mc100_audio_push(a, &frame, true) == MC100_OK);
    frame.seq = bad_seq;
    assert(mc100_audio_push(a, &frame, false) == MC100_CORRUPT);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.stopped && status.cutoff_valid && status.last_accepted_seq == 10);
    assert(status.error == MC100_CORRUPT);
    assert(mc100_audio_stats(a, &stats) == MC100_OK && stats.first_gap_seq == 11);
    assert(stats.first_gap_valid && stats.first_gap_generation == 1);
    assert(mc100_audio_pop(a, &packet) == MC100_OK && packet.frame.seq == 10);
    assert(mc100_audio_pop(a, &packet) == MC100_NOT_READY);
    frame.seq = 11;
    assert(mc100_audio_push(a, &frame, false) == MC100_CORRUPT);
    mc100_audio_destroy(a);
}
static void explicit_gap_and_sequence_exhaustion(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_frame_t frame = {0};
    mc100_audio_status_t status;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    assert(mc100_audio_capture_gap(a, 0) == MC100_IO);
    assert(mc100_audio_push(a, &frame, true) == MC100_IO);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.stopped && !status.cutoff_valid && !status.triggered);
    assert(mc100_audio_arm(a, 2, 0) == MC100_IO);
    mc100_audio_destroy(a);
    a = mc100_audio_create();
    assert(a && mc100_audio_arm(a, 1, UINT64_MAX) == MC100_OK);
    frame.seq = UINT64_MAX;
    assert(mc100_audio_push(a, &frame, true) == MC100_OK);
    frame.seq = 0;
    assert(mc100_audio_push(a, &frame, false) == MC100_CORRUPT);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.last_accepted_seq == UINT64_MAX && status.error == MC100_CORRUPT);
    mc100_audio_destroy(a);
}
int main(void)
{
    assembler_gap_never_fills_missing_audio();
    discontinuity_preserves_only_valid_prefix(12);
    discontinuity_preserves_only_valid_prefix(10);
    discontinuity_preserves_only_valid_prefix(9);
    explicit_gap_and_sequence_exhaustion();
    return 0;
}
