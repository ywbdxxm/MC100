#include <assert.h>
#include <stdio.h>
#include "mc100_record_session.h"

static void target_boundary(void) {
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    assert(mc100_record_session_can_enqueue(&s));
    mc100_record_session_start(&s, 1000);
    assert(mc100_record_session_note_enqueued(&s) == MC100_OK);
    for (uint64_t i = 1; i < MC100_RECORD_TARGET_FRAMES; ++i)
        assert(mc100_record_session_note_enqueued(&s) == MC100_OK);
    assert(mc100_record_session_target_reached(&s));
    assert(mc100_record_session_note_enqueued(&s) == MC100_FULL);
}

static void deadline_is_bounded(void) {
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    mc100_record_session_start(&s, 1000);
    assert(s.deadline_ms == 1000 + 610000);
    assert(!mc100_record_session_deadline_expired(&s, s.deadline_ms - 1));
    assert(mc100_record_session_deadline_expired(&s, s.deadline_ms));
}

static void producer_must_quiesce_before_close(void) {
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    mc100_record_session_start(&s, 0);
    for (uint64_t i = 0; i < MC100_RECORD_TARGET_FRAMES; ++i)
        assert(mc100_record_session_note_enqueued(&s) == MC100_OK);
    assert(!mc100_record_session_can_clean_close(&s));
    mc100_record_session_mark_producer_quiesced(&s);
    assert(!mc100_record_session_can_clean_close(&s));
    for (uint64_t i = 0; i < MC100_RECORD_TARGET_FRAMES; ++i)
        assert(mc100_record_session_note_consumed(&s) == MC100_OK);
    assert(mc100_record_session_can_clean_close(&s));
}

/* One-fill FIFO model for the portable session seam. This does not execute
 * record_loop.c, FreeRTOS scheduling, or I2S; target quiescence and queue
 * interleavings still require a target fault-injection test. */
typedef struct {
    mc100_frame_t frames[MC100_STREAM_FRAMES];
    size_t count;
} stalled_writer_queue_t;

static mc100_result_t stalled_writer_enqueue(stalled_writer_queue_t *queue,
                                             const mc100_frame_t *frame) {
    if (queue->count == sizeof(queue->frames) / sizeof(queue->frames[0]))
        return MC100_FULL;
    queue->frames[queue->count++] = *frame;
    return MC100_OK;
}

static void queue_full_latches_fault_until_quiesced_discard(void) {
    stalled_writer_queue_t queue = {0};
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    mc100_record_session_start(&s, 0);
    for (uint64_t seq = 0; seq < MC100_STREAM_FRAMES; ++seq) {
        mc100_frame_t frame = {.seq = seq};
        assert(mc100_record_session_can_enqueue(&s));
        assert(stalled_writer_enqueue(&queue, &frame) == MC100_OK);
        assert(mc100_record_session_note_enqueued(&s) == MC100_OK);
    }

    /* The 97th frame is rejected by capacity, not the 30,000-frame limit. */
    mc100_frame_t rejected = {.seq = MC100_STREAM_FRAMES};
    assert(mc100_record_session_can_enqueue(&s));
    mc100_result_t result = stalled_writer_enqueue(&queue, &rejected);
    assert(result == MC100_FULL);
    mc100_record_session_mark_fault(&s, result);
    assert(s.faulted && s.fault == MC100_FULL);
    assert(s.enqueued_frames == MC100_STREAM_FRAMES && s.consumed_frames == 0);
    assert(!mc100_record_session_target_reached(&s));
    assert(!mc100_record_session_can_enqueue(&s));
    assert(mc100_record_session_note_enqueued(&s) == MC100_FULL);
    assert(s.enqueued_frames == MC100_STREAM_FRAMES);

    /* A later teardown error must not replace the first queue-full fault. */
    mc100_record_session_mark_fault(&s, MC100_IO);
    assert(s.fault == MC100_FULL);
    assert(!s.producer_quiesced);
    assert(queue.count == MC100_STREAM_FRAMES);
    assert(!mc100_record_session_can_clean_close(&s));

    /* No teardown drain is attempted until the producer acknowledges stop.
     * Faulted frames are discarded, never counted as successfully consumed. */
    mc100_record_session_mark_producer_quiesced(&s);
    assert(s.producer_quiesced);
    assert(!mc100_record_session_can_clean_close(&s));
    size_t discarded = 0;
    for (; discarded < queue.count; ++discarded)
        assert(queue.frames[discarded].seq == discarded);
    queue.count = 0;
    assert(discarded == MC100_STREAM_FRAMES);
    assert(mc100_record_session_note_consumed(&s) == MC100_FULL);
    assert(s.consumed_frames == 0 && s.fault == MC100_FULL);
    assert(!mc100_record_session_can_clean_close(&s));
}

static void fault_forbids_clean_close(void) {
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    mc100_record_session_start(&s, 0);
    for (uint64_t i = 0; i < MC100_RECORD_TARGET_FRAMES; ++i) {
        assert(mc100_record_session_note_enqueued(&s) == MC100_OK);
        assert(mc100_record_session_note_consumed(&s) == MC100_OK);
    }
    mc100_record_session_mark_producer_quiesced(&s);
    mc100_record_session_mark_fault(&s, MC100_IO);
    assert(s.fault == MC100_IO);
    assert(!mc100_record_session_can_clean_close(&s));
}

static void no_frame_cannot_clean_close(void) {
    mc100_record_session_t s;
    mc100_record_session_init(&s);
    mc100_record_session_start(&s, 0);
    mc100_record_session_mark_producer_quiesced(&s);
    assert(!mc100_record_session_can_clean_close(&s));
}

int main(void) {
    target_boundary();
    deadline_is_bounded();
    producer_must_quiesce_before_close();
    queue_full_latches_fault_until_quiesced_discard();
    fault_forbids_clean_close();
    no_frame_cannot_clean_close();
    puts("record_session: lifecycle policy PASS");
    return 0;
}
