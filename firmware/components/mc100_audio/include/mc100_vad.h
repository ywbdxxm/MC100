#ifndef MC100_VAD_H
#define MC100_VAD_H

#include "mc100_types.h"

/*
 * Per-frame voice-activity decision seam. Phase 3 will replace the Phase 2
 * placeholder with the VAD selected by the Phase 1 spike (esp-sr VADNet or
 * libfvad). Stateful implementations keep their state in ctx.
 */
typedef struct {
    bool (*decide)(void *ctx, const mc100_frame_t *frame);
    void *ctx;
} mc100_vad_t;

/*
 * Deterministic bring-up placeholder. It returns true exactly once, for the
 * frame whose sequence number equals trigger_seq, so LISTEN -> RECORD -> close
 * can be exercised without real audio.
 */
typedef struct {
    uint64_t trigger_seq;
    bool fired;
} mc100_vad_fixed_t;

mc100_vad_t mc100_vad_fixed(mc100_vad_fixed_t *state, uint64_t trigger_seq);

#endif
