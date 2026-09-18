#ifndef MC100_TYPES_H
#define MC100_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MC100_SAMPLE_RATE = 16000,
    MC100_FRAME_SAMPLES = 320,
    MC100_PREROLL_FRAMES = 100,
    MC100_STREAM_FRAMES = 96,
    MC100_SEGMENT_FRAMES = 15000
};

typedef uint64_t mc100_generation_t;

typedef enum {
    MC100_OK,
    MC100_INVALID,
    MC100_NOT_READY,
    MC100_FULL,
    MC100_IO,
    MC100_CORRUPT,
    MC100_TIMEOUT
} mc100_result_t;

typedef struct {
    uint64_t seq;
    int16_t pcm[320];
} mc100_frame_t;

typedef struct {
    mc100_generation_t generation;
    mc100_frame_t frame;
} mc100_packet_t;

typedef struct {
    mc100_generation_t generation;
    uint64_t first_seq;
    uint16_t count;
    uint8_t bank_id;
} mc100_snapshot_t;

_Static_assert(sizeof(mc100_frame_t) == 648, "frame RAM budget");
_Static_assert(sizeof(mc100_packet_t) == 656, "stream RAM budget");

#endif
