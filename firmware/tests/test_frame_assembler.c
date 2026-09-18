#include "mc100_audio.h"
#include <assert.h>
#include <string.h>

typedef struct { mc100_frame_t frames[3]; size_t count; bool fail; } sink_t;
static mc100_result_t collect(void *context, const mc100_frame_t *frame)
{
    sink_t *sink = context;
    if (sink->fail) return MC100_IO;
    assert(sink->count < 3);
    sink->frames[sink->count++] = *frame;
    return MC100_OK;
}

/* Catches dropped odd bytes, wrong endian conversion and partition dependence. */
static void fragmented_pcm_is_identical(void)
{
    uint8_t pcm[1920];
    const size_t chunks[] = {1, 3, 639, 640, 641, 1919, 1920};
    for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = (uint8_t)(i * 73u + 19u);
    const size_t fixed_trials = sizeof(chunks) / sizeof(chunks[0]);
    uint32_t seed = UINT32_C(0x31c04e17);
    for (size_t trial = 0; trial < fixed_trials + 32; ++trial) {
        mc100_assembler_t assembler;
        sink_t sink = {0};
        mc100_assembler_init(&assembler, 42);
        for (size_t offset = 0; offset < sizeof(pcm);) {
            seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
            size_t count = trial < fixed_trials ? chunks[trial] : 1 + seed % 997;
            if (count > sizeof(pcm) - offset) count = sizeof(pcm) - offset;
            assert(mc100_assembler_feed(&assembler, pcm + offset, count,
                                        collect, &sink) == MC100_OK);
            offset += count;
        }
        assert(sink.count == 3);
        for (size_t f = 0; f < 3; ++f) {
            assert(sink.frames[f].seq == 42 + f);
            for (size_t k = 0; k < 320; ++k) {
                uint16_t actual = (uint16_t)sink.frames[f].pcm[k];
                assert((uint8_t)actual == pcm[f * 640 + k * 2]);
                assert((uint8_t)(actual >> 8) == pcm[f * 640 + k * 2 + 1]);
            }
        }
    }
}

static void callback_error_and_overflow_latch(void)
{
    uint8_t pcm[1280] = {0};
    mc100_assembler_t assembler;
    sink_t sink = {0};
    mc100_assembler_init(&assembler, 0);
    sink.fail = true;
    assert(mc100_assembler_feed(&assembler, pcm, sizeof(pcm), collect, &sink) == MC100_IO);
    sink.fail = false;
    assert(mc100_assembler_feed(&assembler, pcm, 640, collect, &sink) == MC100_IO);
    assert(sink.count == 0);
    mc100_assembler_init(&assembler, UINT64_MAX);
    assert(mc100_assembler_feed(&assembler, pcm, 640, collect, &sink) == MC100_OK);
    assert(sink.count == 1 && sink.frames[0].seq == UINT64_MAX);
    assert(mc100_assembler_feed(&assembler, pcm, 1, collect, &sink) == MC100_CORRUPT);
    assert(sink.count == 1);
    assert(mc100_assembler_feed(NULL, pcm, 1, collect, &sink) == MC100_INVALID);
    mc100_assembler_init(&assembler, 0);
    assert(mc100_assembler_feed(&assembler, NULL, 1, collect, &sink) == MC100_INVALID);
    assert(mc100_assembler_feed(&assembler, NULL, 0, collect, &sink) == MC100_OK);
}

int main(void)
{
    fragmented_pcm_is_identical();
    callback_error_and_overflow_latch();
    return 0;
}
