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

static void queue_fault_forbids_clean_close(void) {
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
    queue_fault_forbids_clean_close();
    no_frame_cannot_clean_close();
    puts("record_session: lifecycle policy PASS");
    return 0;
}
