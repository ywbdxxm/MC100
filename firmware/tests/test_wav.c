#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc100_format.h"

/* These literals check the on-disk contract, not the encoder's own decoder. */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void)
{
    assert(mc100_crc32("123456789", 9) == UINT32_C(0xCBF43926));
    assert(mc100_crc32(NULL, 0) == 0);
    uint8_t header[512];
    const uint32_t lengths[] = {0, 2, 640, 9600000};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        memset(header, 0xa5, sizeof(header));
        assert(mc100_wav_header(header, lengths[i]) == MC100_OK);
        assert(memcmp(header, "RIFF", 4) == 0);
        assert(read_le32(header + 4) == 504 + lengths[i]);
        assert(memcmp(header + 8, "WAVEfmt ", 8) == 0);
        assert(read_le32(header + 16) == 16);
        assert(header[20] == 1 && header[21] == 0);
        assert(header[22] == 1 && header[23] == 0);
        assert(read_le32(header + 24) == 16000);
        assert(read_le32(header + 28) == 32000);
        assert(header[32] == 2 && header[33] == 0);
        assert(header[34] == 16 && header[35] == 0);
        assert(memcmp(header + 36, "JUNK", 4) == 0);
        assert(read_le32(header + 40) == 460);
        for (size_t k = 44; k < 504; ++k) assert(header[k] == 0);
        assert(memcmp(header + 504, "data", 4) == 0);
        assert(read_le32(header + 508) == lengths[i]);
    }
    const uint32_t invalid[] = {1, 641, 9600002, UINT32_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        memset(header, 0xa5, sizeof(header));
        assert(mc100_wav_header(header, invalid[i]) == MC100_INVALID);
        for (size_t k = 0; k < sizeof(header); ++k) assert(header[k] == 0xa5);
    }
    assert(mc100_wav_header(NULL, 0) == MC100_INVALID);
    puts("wav: CRC vector, PCM layout, bounds and non-mutating rejection PASS");
    return 0;
}
