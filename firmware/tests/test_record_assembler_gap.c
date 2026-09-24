#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "mc100_audio.h"

static mc100_result_t collect(void *context, const mc100_frame_t *frame) {
    (void)context;
    (void)frame;
    return MC100_OK;
}

int main(void) {
    mc100_assembler_t assembler;
    uint8_t pcm[640] = {0};
    mc100_assembler_init(&assembler, 0);
    assert(mc100_assembler_feed(&assembler, pcm, 639, collect, NULL) == MC100_OK);
    assert(mc100_assembler_capture_gap(&assembler) == MC100_IO);
    assert(mc100_assembler_feed(&assembler, pcm, sizeof(pcm), collect, NULL) == MC100_IO);
    puts("record_assembler_gap: capture gap latches PASS");
    return 0;
}
