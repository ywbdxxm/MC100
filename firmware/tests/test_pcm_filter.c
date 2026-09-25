#include "mc100_pcm_filter.h"

#include <assert.h>
#include <string.h>

static void bypass_is_identity_and_tracks_full_negative_peak(void)
{
    int16_t samples[] = {-32768, -1, 0, 1, 32767};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 5, 1, false) == MC100_OK);
    assert(samples[0] == -32768 && samples[1] == -1 && samples[2] == 0);
    assert(samples[3] == 1 && samples[4] == 32767);
    assert(filter.input_peak == 32768 && filter.output_peak == 32768);
    assert(filter.clipped_samples == 0);
}

static void seeded_constant_offset_is_removed(void)
{
    int16_t samples[] = {1000, 1000, 1000, 1000};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 4, 8, true) == MC100_OK);
    for (size_t i = 0; i < 4; ++i) assert(samples[i] == 0);
    assert(filter.input_peak == 1000 && filter.output_peak == 0);
    assert(filter.clipped_samples == 0);
}

static void dc_step_uses_q16_residual_before_conversion(void)
{
    int16_t samples[] = {0, 1000};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 2, 1, true) == MC100_OK);
    assert(samples[0] == 0 && samples[1] == 998);
    assert(filter.input_peak == 1000 && filter.output_peak == 998);
}

static void low_level_step_gains_fractional_residual(void)
{
    int16_t samples[] = {0, 1};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 2, 8, true) == MC100_OK);
    assert(samples[0] == 0 && samples[1] == 7);
}

static void gain_saturates_only_outside_signed_range(void)
{
    int16_t samples[] = {16383, 16384, -16384, -16385};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 4, 2, false) == MC100_OK);
    assert(samples[0] == 32766 && samples[1] == 32767);
    assert(samples[2] == -32768 && samples[3] == -32768);
    assert(filter.input_peak == 16385 && filter.output_peak == 32768);
    assert(filter.clipped_samples == 2);
}

static void invalid_arguments_leave_state_and_samples_unchanged(void)
{
    int16_t samples[] = {20, -30};
    mc100_pcm_filter_t filter;
    mc100_pcm_filter_init(&filter);
    assert(mc100_pcm_filter_process(&filter, samples, 1, 1, true) == MC100_OK);
    mc100_pcm_filter_t before = filter;
    int16_t next = samples[1];
    assert(mc100_pcm_filter_process(NULL, samples, 2, 1, true) == MC100_INVALID);
    assert(mc100_pcm_filter_process(&filter, NULL, 1, 1, true) == MC100_INVALID);
    assert(mc100_pcm_filter_process(&filter, samples, 2, 0, true) == MC100_INVALID);
    assert(mc100_pcm_filter_process(&filter, samples, 2, 17, true) == MC100_INVALID);
    assert(memcmp(&filter, &before, sizeof(filter)) == 0 && samples[1] == next);
    assert(mc100_pcm_filter_process(&filter, NULL, 0, 1, true) == MC100_OK);
    assert(memcmp(&filter, &before, sizeof(filter)) == 0);
}

static void chunks_match_whole_buffer_and_reinit_clears_history(void)
{
    int16_t whole[] = {0, 1000, -1000, 32767, -32768, 1, 1};
    int16_t chunks[] = {0, 1000, -1000, 32767, -32768, 1, 1};
    mc100_pcm_filter_t a, b;
    mc100_pcm_filter_init(&a);
    mc100_pcm_filter_init(&b);
    assert(mc100_pcm_filter_process(&a, whole, 7, 8, true) == MC100_OK);
    assert(mc100_pcm_filter_process(&b, chunks, 2, 8, true) == MC100_OK);
    assert(mc100_pcm_filter_process(&b, chunks + 2, 3, 8, true) == MC100_OK);
    assert(mc100_pcm_filter_process(&b, chunks + 5, 2, 8, true) == MC100_OK);
    assert(memcmp(whole, chunks, sizeof(whole)) == 0);
    assert(a.dc_q16 == b.dc_q16 && a.seeded == b.seeded);
    assert(a.input_peak == b.input_peak && a.output_peak == b.output_peak);
    assert(a.clipped_samples == b.clipped_samples);
    mc100_pcm_filter_init(&b);
    assert(b.dc_q16 == 0 && !b.seeded && b.input_peak == 0);
    assert(b.output_peak == 0 && b.clipped_samples == 0);
}

int main(void)
{
    bypass_is_identity_and_tracks_full_negative_peak();
    seeded_constant_offset_is_removed();
    dc_step_uses_q16_residual_before_conversion();
    low_level_step_gains_fractional_residual();
    gain_saturates_only_outside_signed_range();
    invalid_arguments_leave_state_and_samples_unchanged();
    chunks_match_whole_buffer_and_reinit_clears_history();
    return 0;
}
