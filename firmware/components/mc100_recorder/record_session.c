#include "mc100_record_session.h"
#include <limits.h>
#include <string.h>

static bool valid(const mc100_record_session_t *session) {
    return session != NULL;
}

void mc100_record_session_init(mc100_record_session_t *session) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    session->target_frames = MC100_RECORD_TARGET_FRAMES;
    session->fault = MC100_OK;
}

void mc100_record_session_start(mc100_record_session_t *session,
                                uint64_t first_frame_ms) {
    if (!valid(session)) return;
    session->started = true;
    session->producer_quiesced = false;
    session->deadline_ms = first_frame_ms;
    const uint64_t duration_ms =
        (uint64_t)MC100_RECORD_DEADLINE_SECONDS * UINT64_C(1000);
    if (UINT64_MAX - first_frame_ms < duration_ms)
        session->deadline_ms = UINT64_MAX;
    else
        session->deadline_ms = first_frame_ms + duration_ms;
}

bool mc100_record_session_can_enqueue(
    const mc100_record_session_t *session) {
    return valid(session) && session->started && !session->producer_quiesced &&
           !session->faulted &&
           session->enqueued_frames < session->target_frames;
}

mc100_result_t mc100_record_session_note_enqueued(
    mc100_record_session_t *session) {
    if (!valid(session)) return MC100_INVALID;
    if (session->faulted) return session->fault;
    if (!session->started || session->producer_quiesced)
        return MC100_NOT_READY;
    if (session->enqueued_frames >= session->target_frames) return MC100_FULL;
    ++session->enqueued_frames;
    return MC100_OK;
}

mc100_result_t mc100_record_session_note_consumed(
    mc100_record_session_t *session) {
    if (!valid(session)) return MC100_INVALID;
    if (session->faulted) return session->fault;
    if (!session->started) return MC100_NOT_READY;
    if (session->consumed_frames >= session->enqueued_frames)
        return MC100_INVALID;
    ++session->consumed_frames;
    return MC100_OK;
}

void mc100_record_session_mark_producer_quiesced(
    mc100_record_session_t *session) {
    if (!valid(session)) return;
    session->producer_quiesced = true;
}

void mc100_record_session_mark_fault(mc100_record_session_t *session,
                                     mc100_result_t reason) {
    if (!valid(session) || session->faulted) return;
    session->faulted = true;
    session->fault = reason;
}

bool mc100_record_session_target_reached(
    const mc100_record_session_t *session) {
    return valid(session) && session->enqueued_frames >= session->target_frames;
}

bool mc100_record_session_deadline_expired(
    const mc100_record_session_t *session, uint64_t now_ms) {
    return valid(session) && session->started && now_ms >= session->deadline_ms;
}

bool mc100_record_session_can_clean_close(
    const mc100_record_session_t *session) {
    return valid(session) && session->started && !session->faulted &&
           mc100_record_session_target_reached(session) &&
           session->producer_quiesced &&
           session->consumed_frames == session->enqueued_frames;
}
