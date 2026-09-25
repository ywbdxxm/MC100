#ifndef MC100_RECORD_SESSION_H
#define MC100_RECORD_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include "mc100_record_settings.h"
#include "mc100_types.h"

enum {
    MC100_RECORD_FRAME_MS = 20,
    MC100_RECORD_TARGET_FRAMES =
        MC100_RECORD_DURATION_SECONDS * 1000 / MC100_RECORD_FRAME_MS,
    MC100_RECORD_DEADLINE_SECONDS = MC100_RECORD_DURATION_SECONDS + 10
};

typedef struct {
    uint64_t target_frames;
    uint64_t enqueued_frames;
    uint64_t consumed_frames;
    uint64_t deadline_ms;
    bool started;
    bool producer_quiesced;
    bool faulted;
    mc100_result_t fault;
} mc100_record_session_t;

void mc100_record_session_init(mc100_record_session_t *session);
void mc100_record_session_start(mc100_record_session_t *session,
                                uint64_t first_frame_ms);
bool mc100_record_session_can_enqueue(
    const mc100_record_session_t *session);
mc100_result_t mc100_record_session_note_enqueued(
    mc100_record_session_t *session);
mc100_result_t mc100_record_session_note_consumed(
    mc100_record_session_t *session);
void mc100_record_session_mark_producer_quiesced(
    mc100_record_session_t *session);
void mc100_record_session_mark_fault(mc100_record_session_t *session,
                                     mc100_result_t reason);
bool mc100_record_session_target_reached(
    const mc100_record_session_t *session);
bool mc100_record_session_deadline_expired(
    const mc100_record_session_t *session, uint64_t now_ms);
bool mc100_record_session_can_clean_close(
    const mc100_record_session_t *session);

#endif
