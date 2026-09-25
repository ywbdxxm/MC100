#ifndef MC100_PCM_FILTER_H
#define MC100_PCM_FILTER_H

#include "mc100_types.h"

typedef struct {
    int64_t dc_q16;
    uint32_t input_peak;
    uint32_t output_peak;
    uint64_t clipped_samples;
    bool seeded;
} mc100_pcm_filter_t;

void mc100_pcm_filter_init(mc100_pcm_filter_t *filter);
mc100_result_t mc100_pcm_filter_process(mc100_pcm_filter_t *filter,
                                        int16_t *samples, size_t count,
                                        uint32_t gain, bool dc_block);

#endif
