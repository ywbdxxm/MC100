#include "mc100_vad.h"

static bool fixed_decide(void *ctx, const mc100_frame_t *frame)
{
    mc100_vad_fixed_t *state = ctx;

    if (!state->fired && frame->seq == state->trigger_seq) {
        state->fired = true;
        return true;
    }
    return false;
}

mc100_vad_t mc100_vad_fixed(mc100_vad_fixed_t *state, uint64_t trigger_seq)
{
    state->trigger_seq = trigger_seq;
    state->fired = false;
    mc100_vad_t vad = {fixed_decide, state};
    return vad;
}
