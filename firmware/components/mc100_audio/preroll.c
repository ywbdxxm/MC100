#include "audio_internal.h"

void mc100_audio_roll(mc100_audio_t *a, const mc100_frame_t *frame)
{
    mc100_bank_t *bank = &a->banks[a->rolling];
    uint16_t tail = (uint16_t)((bank->start + bank->count) % MC100_PREROLL_FRAMES);
    bank->frames[tail] = *frame;
    if (bank->count < MC100_PREROLL_FRAMES) ++bank->count;
    else bank->start = (uint16_t)((bank->start + 1) % MC100_PREROLL_FRAMES);
}

void mc100_audio_freeze(mc100_audio_t *a, mc100_audio_session_t *session,
                       uint64_t trigger_seq)
{
    mc100_bank_t *bank = &a->banks[a->rolling];
    uint64_t first = trigger_seq > MC100_PREROLL_FRAMES ?
                     trigger_seq - MC100_PREROLL_FRAMES : 0;
    if (session->min_seq > first) first = session->min_seq;
    while (bank->count && bank->frames[bank->start].seq < first) {
        bank->start = (uint16_t)((bank->start + 1) % MC100_PREROLL_FRAMES);
        --bank->count;
    }
    bank->state = BANK_FROZEN;
    bank->generation = session->generation;
    bank->first_seq = bank->count ? bank->frames[bank->start].seq : trigger_seq;
    session->bank_id = a->rolling;
    session->snapshot_owned = true;
    session->status.triggered = true;
    session->status.first_live_seq = trigger_seq;
    /* Freezing accepts this valid prefix even if the very first live packet
     * subsequently encounters a full FIFO shared with the closing session. */
    if (bank->count) {
        session->status.cutoff_valid = true;
        session->status.last_accepted_seq =
            bank->frames[(bank->start + bank->count - 1) % MC100_PREROLL_FRAMES].seq;
    }
    a->rolling ^= 1;
    bank = &a->banks[a->rolling];
    bank->state = BANK_ROLLING;
    bank->start = 0;
    bank->count = 0;
    bank->generation = 0;
}

mc100_result_t mc100_audio_snapshot(mc100_audio_t *a,
                                   mc100_generation_t generation,
                                   mc100_snapshot_t *snapshot)
{
    if (!a || !snapshot || !generation) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, generation);
    if (!session) return MC100_INVALID;
    if (!session->status.triggered) return MC100_NOT_READY;
    if (!session->snapshot_owned) return MC100_INVALID;
    const mc100_bank_t *bank = &a->banks[session->bank_id];
    snapshot->generation = generation;
    snapshot->first_seq = bank->first_seq;
    snapshot->count = bank->count;
    snapshot->bank_id = session->bank_id;
    return MC100_OK;
}

mc100_result_t mc100_audio_snapshot_frame(mc100_audio_t *a,
                                         const mc100_snapshot_t *snapshot,
                                         uint16_t index, mc100_frame_t *frame)
{
    if (!a || !snapshot || !frame || snapshot->bank_id >= 2) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, snapshot->generation);
    if (!session || !session->snapshot_owned || session->bank_id != snapshot->bank_id)
        return MC100_INVALID;
    const mc100_bank_t *bank = &a->banks[snapshot->bank_id];
    if (bank->state != BANK_FROZEN || bank->generation != snapshot->generation ||
        bank->first_seq != snapshot->first_seq || bank->count != snapshot->count ||
        index >= bank->count) return MC100_INVALID;
    *frame = bank->frames[(bank->start + index) % MC100_PREROLL_FRAMES];
    return MC100_OK;
}

mc100_result_t mc100_audio_snapshot_release(mc100_audio_t *a,
                                           mc100_generation_t generation)
{
    if (!a || !generation) return MC100_INVALID;
    mc100_audio_session_t *session = mc100_audio_find(a, generation);
    if (!session || !session->snapshot_owned) return MC100_INVALID;
    a->banks[session->bank_id].state = BANK_FREE;
    session->snapshot_owned = false;
    return MC100_OK;
}
