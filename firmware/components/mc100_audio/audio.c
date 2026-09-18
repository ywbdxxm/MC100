#include "audio_internal.h"
#include <stdlib.h>
#include <string.h>

mc100_audio_t *mc100_audio_create(void)
{
    mc100_audio_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->active = -1;
    a->banks[0].state = BANK_ROLLING;
    return a;
}

void mc100_audio_destroy(mc100_audio_t *a)
{
    free(a);
}

mc100_audio_session_t *mc100_audio_find(mc100_audio_t *a, mc100_generation_t generation)
{
    for (size_t i = 0; i < 2; ++i)
        if (a->sessions[i].used && a->sessions[i].generation == generation)
            return &a->sessions[i];
    return NULL;
}

mc100_result_t mc100_audio_arm(mc100_audio_t *a, mc100_generation_t generation,
                              uint64_t min_seq)
{
    if (!a || !generation || generation <= a->last_generation) return MC100_INVALID;
    if (a->error != MC100_OK) return a->error;
    if (a->sequence_valid && a->last_seq == UINT64_MAX) return MC100_CORRUPT;
    if (a->active >= 0 || a->banks[a->rolling ^ 1].state != BANK_FREE)
        return MC100_NOT_READY;
    for (int i = 0; i < 2; ++i) {
        if (!a->sessions[i].used) {
            mc100_audio_session_t *session = &a->sessions[i];
            memset(session, 0, sizeof(*session));
            session->used = true;
            session->generation = generation;
            session->min_seq = min_seq;
            a->active = i;
            a->last_generation = generation;
            return MC100_OK;
        }
    }
    return MC100_NOT_READY;
}

static mc100_result_t latch_error(mc100_audio_t *a, mc100_result_t error,
                                  uint64_t first_gap_seq)
{
    if (a->error != MC100_OK) return a->error;
    a->error = error;
    mc100_audio_stats_t *stats = &a->stream.stats;
    stats->first_gap_valid = true;
    stats->first_gap_seq = first_gap_seq;
    if (a->active >= 0) {
        mc100_audio_session_t *session = &a->sessions[a->active];
        stats->first_gap_generation = session->generation;
        session->status.stopped = true;
        session->status.error = error;
        a->active = -1;
    }
    return error;
}

mc100_result_t mc100_audio_push(mc100_audio_t *a, const mc100_frame_t *frame,
                               bool trigger)
{
    if (!a || !frame) return MC100_INVALID;
    if (a->error != MC100_OK) return a->error;
    if (a->sequence_valid) {
        if (a->last_seq == UINT64_MAX) return latch_error(a, MC100_CORRUPT, UINT64_MAX);
        if (frame->seq != a->last_seq + 1) return latch_error(a, MC100_CORRUPT, a->last_seq + 1);
    }
    a->last_seq = frame->seq;
    a->sequence_valid = true;
    if (a->active < 0) {
        if (trigger) a->pending_trigger = true;
    } else {
        mc100_audio_session_t *session = &a->sessions[a->active];
        if (!session->status.triggered && (trigger || a->pending_trigger)) {
            a->pending_trigger = true;
            if (frame->seq >= session->min_seq) {
                /* ARM reserved the free second bank; no operation can steal it. */
                mc100_audio_freeze(a, session, frame->seq);
                a->pending_trigger = false;
            }
        }
        if (session->status.triggered) {
            mc100_result_t result = mc100_stream_push(&a->stream, session->generation, frame);
            if (result != MC100_OK) return latch_error(a, result, frame->seq);
            session->status.cutoff_valid = true;
            session->status.last_accepted_seq = frame->seq;
        }
    }
    mc100_audio_roll(a, frame);
    return MC100_OK;
}

mc100_result_t mc100_audio_stop(mc100_audio_t *a, mc100_generation_t generation,
                               uint64_t *last_accepted_seq)
{
    if (!a || !generation || !last_accepted_seq) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, generation);
    if (!session) return MC100_INVALID;
    session->status.stopped = true;
    if (a->active >= 0 && &a->sessions[a->active] == session) a->active = -1;
    *last_accepted_seq = session->status.last_accepted_seq;
    return MC100_OK;
}

mc100_result_t mc100_audio_status(mc100_audio_t *a, mc100_generation_t generation,
                                 mc100_audio_status_t *status)
{
    if (!a || !generation || !status) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, generation);
    if (!session) return MC100_INVALID;
    *status = session->status;
    return MC100_OK;
}

mc100_result_t mc100_audio_release(mc100_audio_t *a,
                                  mc100_generation_t generation)
{
    if (!a || !generation) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, generation);
    if (!session) return MC100_INVALID;
    if (session->snapshot_owned) {
        a->banks[session->bank_id].state = BANK_FREE;
        session->snapshot_owned = false;
    }
    if (a->active >= 0 && &a->sessions[a->active] == session) a->active = -1;
    mc100_stream_discard(&a->stream, generation);
    session->used = false;
    return MC100_OK;
}

mc100_result_t mc100_audio_capture_gap(mc100_audio_t *a, uint64_t first_gap_seq)
{
    if (!a) return MC100_INVALID;
    return latch_error(a, MC100_IO, first_gap_seq);
}
