#include "mc100_audio.h"
#include <string.h>

void mc100_assembler_init(mc100_assembler_t *assembler, uint64_t initial_seq)
{
    if (!assembler) return;
    memset(assembler, 0, sizeof(*assembler));
    assembler->frame.seq = initial_seq;
}

mc100_result_t mc100_assembler_feed(mc100_assembler_t *assembler,
                                   const uint8_t *bytes, size_t length,
                                   mc100_frame_callback_t callback, void *context)
{
    if (!assembler || (!bytes && length) || !callback) return MC100_INVALID;
    if (assembler->error != MC100_OK) return assembler->error;
    for (size_t i = 0; i < length; ++i) {
        if (assembler->sequence_exhausted) {
            assembler->error = MC100_CORRUPT;
            return assembler->error;
        }
        if ((assembler->used_bytes & 1u) == 0) {
            assembler->low_byte = bytes[i];
        } else {
            uint16_t bits = (uint16_t)((uint16_t)bytes[i] * 256u + assembler->low_byte);
            int32_t sample = bits;
            if (sample >= 32768) sample -= 65536;
            assembler->frame.pcm[assembler->used_bytes / 2] = (int16_t)sample;
        }
        ++assembler->used_bytes;
        if (assembler->used_bytes == MC100_FRAME_SAMPLES * 2u) {
            mc100_result_t result = callback(context, &assembler->frame);
            if (result != MC100_OK) {
                assembler->error = result;
                return result;
            }
            assembler->used_bytes = 0;
            if (assembler->frame.seq == UINT64_MAX) assembler->sequence_exhausted = true;
            else ++assembler->frame.seq;
        }
    }
    return MC100_OK;
}

mc100_result_t mc100_assembler_capture_gap(mc100_assembler_t *assembler)
{
    if (!assembler) return MC100_INVALID;
    if (assembler->error == MC100_OK) assembler->error = MC100_IO;
    return assembler->error;
}
