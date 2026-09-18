#ifndef MC100_AUDIO_INTERNAL_H
#define MC100_AUDIO_INTERNAL_H

#include "mc100_audio.h"

typedef enum { BANK_FREE, BANK_ROLLING, BANK_FROZEN } mc100_bank_state_t;
typedef struct {
    mc100_frame_t frames[MC100_PREROLL_FRAMES];
    mc100_bank_state_t state;
    uint16_t start;
    uint16_t count;
    mc100_generation_t generation;
    uint64_t first_seq;
} mc100_bank_t;
typedef struct {
    bool used;
    bool snapshot_owned;
    uint8_t bank_id;
    mc100_generation_t generation;
    uint64_t min_seq;
    mc100_audio_status_t status;
} mc100_audio_session_t;
typedef struct {
    mc100_packet_t packets[MC100_STREAM_FRAMES];
    uint16_t head;
    mc100_audio_stats_t stats;
} mc100_stream_t;
struct mc100_audio {
    mc100_bank_t banks[2];
    mc100_stream_t stream;
    mc100_audio_session_t sessions[2];
    int active;
    uint8_t rolling;
    mc100_generation_t last_generation;
    uint64_t last_seq;
    bool sequence_valid;
    bool pending_trigger;
    mc100_result_t error;
};

_Static_assert(sizeof(((mc100_bank_t *)0)->frames) * 2 == 129600,
               "two preroll bank RAM budget");
_Static_assert(sizeof(((mc100_stream_t *)0)->packets) == 62976,
               "96-packet FIFO RAM budget");
_Static_assert(sizeof(mc100_audio_t) <= 194000, "bounded audio context RAM");

mc100_audio_session_t *mc100_audio_find(mc100_audio_t *a, mc100_generation_t generation);
mc100_result_t mc100_stream_push(mc100_stream_t *stream, mc100_generation_t generation,
                                 const mc100_frame_t *frame);
void mc100_stream_discard(mc100_stream_t *stream, mc100_generation_t generation);
void mc100_audio_roll(mc100_audio_t *a, const mc100_frame_t *frame);
void mc100_audio_freeze(mc100_audio_t *a, mc100_audio_session_t *session,
                       uint64_t trigger_seq);

#endif
