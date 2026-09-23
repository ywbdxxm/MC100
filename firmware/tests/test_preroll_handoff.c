#include "mc100_audio.h"
#include <assert.h>
#include <string.h>

static mc100_frame_t make_frame(uint64_t seq)
{
    mc100_frame_t f = {0};
    f.seq = seq;
    for (size_t k = 0; k < 320; ++k) f.pcm[k] = (int16_t)((seq * 17 + k) % 32768);
    return f;
}
static void push(mc100_audio_t *a, uint64_t seq, bool trigger)
{
    mc100_frame_t f = make_frame(seq);
    assert(mc100_audio_push(a, &f, trigger) == MC100_OK);
}
static void check_snapshot(mc100_audio_t *a, mc100_generation_t generation,
                           uint64_t first, uint16_t count)
{
    mc100_snapshot_t snap;
    mc100_frame_t actual;
    assert(mc100_audio_snapshot(a, generation, &snap) == MC100_OK);
    assert(snap.generation == generation && snap.first_seq == first && snap.count == count);
    for (uint16_t i = 0; i < count; ++i) {
        mc100_frame_t expected = make_frame(first + i);
        assert(mc100_audio_snapshot_frame(a, &snap, i, &actual) == MC100_OK);
        assert(actual.seq == expected.seq);
        assert(memcmp(actual.pcm, expected.pcm, sizeof(actual.pcm)) == 0);
    }
    assert(mc100_audio_snapshot_frame(a, &snap, count, &actual) == MC100_INVALID);
    mc100_snapshot_t bad = snap;
    bad.first_seq++;
    assert(mc100_audio_snapshot_frame(a, &bad, 0, &actual) == MC100_INVALID);
    bad = snap; bad.count++;
    assert(mc100_audio_snapshot_frame(a, &bad, 0, &actual) == MC100_INVALID);
    bad = snap; bad.generation++;
    assert(mc100_audio_snapshot_frame(a, &bad, 0, &actual) == MC100_INVALID);
    bad = snap; bad.bank_id ^= 1;
    assert(mc100_audio_snapshot_frame(a, &bad, 0, &actual) == MC100_INVALID);
}
static void exact_history(uint64_t trigger_seq, uint64_t expected_first, uint16_t count)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_packet_t packet;
    mc100_audio_status_t status;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i <= trigger_seq; ++i) push(a, i, i == trigger_seq);
    check_snapshot(a, 1, expected_first, count);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.triggered && status.first_live_seq == trigger_seq);
    assert(mc100_audio_pop(a, &packet) == MC100_OK);
    mc100_frame_t expected = make_frame(trigger_seq);
    assert(packet.generation == 1 && packet.frame.seq == trigger_seq);
    assert(memcmp(packet.frame.pcm, expected.pcm, sizeof(expected.pcm)) == 0);
    assert(mc100_audio_pop(a, &packet) == MC100_NOT_READY);
    mc100_audio_destroy(a);
}

/* Catches bank overwrite, closing queue invalidation, missed unarmed trigger,
 * min-seq overlap, release coupling and stale release affecting successor. */
static void closing_handoff(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_snapshot_t old;
    mc100_frame_t frame;
    mc100_packet_t packet;
    uint64_t cutoff;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    for (uint64_t i = 0; i <= 104; ++i) push(a, i, i == 100);
    assert(mc100_audio_snapshot(a, 1, &old) == MC100_OK);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 104);
    push(a, 105, false);
    push(a, 106, true);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 104);
    assert(mc100_audio_arm(a, 2, 105) == MC100_NOT_READY);
    check_snapshot(a, 1, 0, 100);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_OK);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_INVALID);
    assert(mc100_audio_snapshot_frame(a, &old, 0, &frame) == MC100_INVALID);
    assert(mc100_audio_arm(a, 2, 105) == MC100_OK);
    push(a, 107, false);
    check_snapshot(a, 2, 105, 2);
    assert(mc100_audio_stop(a, 2, &cutoff) == MC100_OK && cutoff == 107);
    assert(mc100_audio_snapshot_release(a, 2) == MC100_OK);
    assert(mc100_audio_arm(a, 3, 108) == MC100_NOT_READY); /* two sessions */
    for (uint64_t i = 100; i <= 104; ++i) {
        assert(mc100_audio_pop(a, &packet) == MC100_OK);
        assert(packet.generation == 1 && packet.frame.seq == i);
    }
    assert(mc100_audio_release(a, 1) == MC100_OK);
    assert(mc100_audio_release(a, 1) == MC100_INVALID);
    assert(mc100_audio_pop(a, &packet) == MC100_OK);
    assert(packet.generation == 2 && packet.frame.seq == 107);
    assert(mc100_audio_release(a, 2) == MC100_OK);
    assert(mc100_audio_arm(a, 3, 108) == MC100_OK);
    push(a, 108, true);
    check_snapshot(a, 3, 108, 0);
    assert(mc100_audio_snapshot_frame(a, &old, 0, &frame) == MC100_INVALID);
    mc100_audio_destroy(a);
}

static void cancel_only_one_generation_and_empty_cutoff(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_audio_status_t status;
    mc100_packet_t packet;
    uint64_t cutoff = 77;
    assert(a && mc100_audio_arm(a, 0, 0) == MC100_INVALID);
    assert(mc100_audio_arm(a, 1, 0) == MC100_OK);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK);
    assert(status.stopped && !status.triggered && !status.cutoff_valid);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK && !status.cutoff_valid);
    assert(mc100_audio_release(a, 1) == MC100_OK);
    assert(mc100_audio_arm(a, 1, 0) == MC100_INVALID);
    assert(mc100_audio_arm(a, 2, 0) == MC100_OK);
    push(a, 0, true);
    assert(mc100_audio_stop(a, 2, &cutoff) == MC100_OK && cutoff == 0);
    assert(mc100_audio_status(a, 2, &status) == MC100_OK && status.cutoff_valid);
    assert(mc100_audio_snapshot_release(a, 2) == MC100_OK);
    assert(mc100_audio_arm(a, 3, 1) == MC100_OK);
    push(a, 1, true);
    assert(mc100_audio_release(a, 2) == MC100_OK); /* explicit cancel queued old */
    assert(mc100_audio_pop(a, &packet) == MC100_OK && packet.generation == 3);
    assert(packet.frame.seq == 1);
    assert(mc100_audio_release(a, 3) == MC100_OK);
    assert(mc100_audio_arm(a, UINT64_MAX, 2) == MC100_OK);
    assert(mc100_audio_release(a, UINT64_MAX) == MC100_OK);
    assert(mc100_audio_arm(a, 1, 2) == MC100_INVALID);
    mc100_audio_destroy(a);
}

static void peek_does_not_consume_successor_packet(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_packet_t packet;
    uint64_t cutoff;
    assert(a && mc100_audio_arm(a, 1, 0) == MC100_OK);
    push(a, 0, true);
    assert(mc100_audio_stop(a, 1, &cutoff) == MC100_OK && cutoff == 0);
    assert(mc100_audio_snapshot_release(a, 1) == MC100_OK);
    assert(mc100_audio_arm(a, 2, 1) == MC100_OK);
    push(a, 1, true);

    assert(mc100_audio_peek(a, &packet) == MC100_OK);
    assert(packet.generation == 1 && packet.frame.seq == 0);
    assert(mc100_audio_peek(a, &packet) == MC100_OK);
    assert(packet.generation == 1 && packet.frame.seq == 0);
    assert(mc100_audio_pop(a, &packet) == MC100_OK);
    assert(packet.generation == 1 && packet.frame.seq == 0);
    assert(mc100_audio_peek(a, &packet) == MC100_OK);
    assert(packet.generation == 2 && packet.frame.seq == 1);
    assert(mc100_audio_pop(a, &packet) == MC100_OK);
    assert(packet.generation == 2 && packet.frame.seq == 1);
    assert(mc100_audio_peek(a, &packet) == MC100_NOT_READY);
    assert(mc100_audio_release(a, 1) == MC100_OK);
    assert(mc100_audio_release(a, 2) == MC100_OK);
    mc100_audio_destroy(a);
}

static void pending_edge_survives_until_minimum_sequence(void)
{
    mc100_audio_t *a = mc100_audio_create();
    mc100_audio_status_t status;
    mc100_packet_t packet;
    assert(a);
    push(a, 0, true);
    push(a, 1, false);
    assert(mc100_audio_arm(a, 1, 4) == MC100_OK);
    push(a, 2, false);
    push(a, 3, false);
    assert(mc100_audio_status(a, 1, &status) == MC100_OK && !status.triggered);
    assert(mc100_audio_pop(a, &packet) == MC100_NOT_READY);
    push(a, 4, false);
    check_snapshot(a, 1, 4, 0);
    assert(mc100_audio_pop(a, &packet) == MC100_OK && packet.frame.seq == 4);
    mc100_audio_destroy(a);
}

int main(void)
{
    exact_history(100, 0, 100);
    exact_history(17, 0, 17);
    exact_history(213, 113, 100);
    exact_history(0, 0, 0);
    closing_handoff();
    cancel_only_one_generation_and_empty_cutoff();
    peek_does_not_consume_successor_packet();
    pending_edge_survives_until_minimum_sequence();
    return 0;
}
