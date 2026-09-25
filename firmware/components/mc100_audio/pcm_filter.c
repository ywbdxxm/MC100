#include "mc100_pcm_filter.h"

#include <string.h>

void mc100_pcm_filter_init(mc100_pcm_filter_t *filter)
{
    if (filter) memset(filter, 0, sizeof(*filter));
}

mc100_result_t mc100_pcm_filter_process(mc100_pcm_filter_t *filter,
                                        int16_t *samples, size_t count,
                                        uint32_t gain, bool dc_block)
{
    if (!filter || (!samples && count) || gain < 1 || gain > 16)
        return MC100_INVALID;

    for (size_t i = 0; i < count; ++i) {
        int64_t sample_q16 = (int64_t)samples[i] * INT64_C(65536);
        int32_t input = samples[i];
        uint32_t input_magnitude = (uint32_t)(input < 0 ? -input : input);
        if (input_magnitude > filter->input_peak)
            filter->input_peak = input_magnitude;

        if (!filter->seeded) {
            filter->dc_q16 = sample_q16;
            filter->seeded = true;
        }
        filter->dc_q16 += (sample_q16 - filter->dc_q16) / 512;

        int64_t residual_q16 = dc_block ? sample_q16 - filter->dc_q16 : sample_q16;
        int64_t amplified = residual_q16 * gain / INT64_C(65536);
        if (amplified > INT16_MAX) {
            amplified = INT16_MAX;
            ++filter->clipped_samples;
        } else if (amplified < INT16_MIN) {
            amplified = INT16_MIN;
            ++filter->clipped_samples;
        }
        samples[i] = (int16_t)amplified;
        int32_t output = samples[i];
        uint32_t output_magnitude = (uint32_t)(output < 0 ? -output : output);
        if (output_magnitude > filter->output_peak)
            filter->output_peak = output_magnitude;
    }
    return MC100_OK;
}
