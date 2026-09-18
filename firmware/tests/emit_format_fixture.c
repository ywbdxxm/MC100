#include <stdio.h>

#include "mc100_format.h"

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    uint8_t header[512];
    if (mc100_wav_header(header, 640) != MC100_OK) return 3;
    FILE *file = fopen(argv[1], "wb");
    if (file == NULL) return 4;
    int result = 0;
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) result = 5;
    for (unsigned sample = 0; sample < 320 && result == 0; ++sample) {
        const uint8_t pcm[2] = {(uint8_t)sample, (uint8_t)(sample >> 8)};
        if (fwrite(pcm, 1, sizeof(pcm), file) != sizeof(pcm)) result = 5;
    }
    if (fclose(file) != 0) result = 6;
    return result;
}
