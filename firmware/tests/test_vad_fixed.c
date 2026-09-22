/*
 * Host contract test for the deterministic VAD placeholder.
 * The implementation is intentionally added after this test in the RED step.
 */
#include "mc100_vad.h"
#include <assert.h>

int main(void)
{
    mc100_vad_fixed_t st;
    mc100_vad_t vad = mc100_vad_fixed(&st, 50);
    mc100_frame_t f = {0};

    f.seq = 49;
    assert(vad.decide(vad.ctx, &f) == false);
    f.seq = 50;
    assert(vad.decide(vad.ctx, &f) == true);
    f.seq = 51;
    assert(vad.decide(vad.ctx, &f) == false);
    f.seq = 50;
    assert(vad.decide(vad.ctx, &f) == false);
    return 0;
}
