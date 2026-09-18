#include <string.h>

#include "mc100_format.h"
#include "format_bytes.h"

mc100_result_t mc100_wav_header(uint8_t out[512], uint32_t pcm_bytes)
{
    if (out == NULL || pcm_bytes > MC100_MAX_PCM_BYTES || (pcm_bytes & 1u)) {
        return MC100_INVALID;
    }
    memset(out, 0, MC100_WAV_HEADER_BYTES);
    memcpy(out, "RIFF", 4);
    mc100_put_u32(out + 4, 504u + pcm_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    mc100_put_u32(out + 16, 16);
    mc100_put_u16(out + 20, 1);
    mc100_put_u16(out + 22, 1);
    mc100_put_u32(out + 24, MC100_SAMPLE_RATE);
    mc100_put_u32(out + 28, MC100_SAMPLE_RATE * 2);
    mc100_put_u16(out + 32, 2);
    mc100_put_u16(out + 34, 16);
    memcpy(out + 36, "JUNK", 4);
    mc100_put_u32(out + 40, 460);
    memcpy(out + 504, "data", 4);
    mc100_put_u32(out + 508, pcm_bytes);
    return MC100_OK;
}
