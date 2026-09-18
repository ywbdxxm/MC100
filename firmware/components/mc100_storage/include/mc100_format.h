#ifndef MC100_FORMAT_H
#define MC100_FORMAT_H

#include "mc100_types.h"

enum {
    MC100_WAV_HEADER_BYTES = 512,
    MC100_INDEX_HEADER_BYTES = 512,
    MC100_INDEX_RECORD_BYTES = 64,
    MC100_INDEX_MAX_RECORDS = 4096,
    MC100_PCM_BLOCK_BYTES = 4096,
    MC100_MAX_PCM_BYTES = 9600000,
    MC100_INDEX_RESERVED = 1,
    MC100_INDEX_CLAIMED = 2,
    MC100_INDEX_INCOMPLETE = 1
};

typedef enum {
    MC100_INDEX_BLOCK = 1,
    MC100_INDEX_CHECKPOINT = 2,
    MC100_INDEX_FINAL = 3,
    MC100_INDEX_INCIDENT = 4
} mc100_index_type_t;

typedef enum {
    MC100_INCIDENT_MIC_IO = 1,
    MC100_INCIDENT_STORAGE_IO = 2,
    MC100_INCIDENT_STORAGE_FULL = 3,
    MC100_INCIDENT_QUEUE_OVERFLOW = 4,
    MC100_INCIDENT_ADC_INVALID = 5,
    MC100_INCIDENT_CONFIG_INVALID = 6,
    MC100_INCIDENT_RECOVERY_REQUIRED = 7,
    MC100_INCIDENT_INTERNAL_PROTOCOL = 8,
    MC100_INCIDENT_LOW_BAT_INTERRUPTED = 9
} mc100_incident_reason_t;

typedef struct {
    uint16_t type;
    uint64_t journal_seq;
    uint64_t pcm_offset;
    uint64_t first_source_sample;
    uint32_t valid_bytes;
    uint32_t payload_crc32;
    mc100_generation_t generation;
    uint32_t flags;
    uint32_t detail;
} mc100_index_record_t;

typedef struct {
    uint32_t flags;
    uint8_t boot_id[16];
    mc100_generation_t generation;
    uint32_t segment_index;
    uint64_t first_source_sample;
} mc100_index_header_t;

typedef struct {
    mc100_generation_t generation;
    uint64_t first_source_sample;
    uint64_t pcm_bytes;
    uint32_t record_count;
    bool terminal;
} mc100_index_validation_t;

/* data may be NULL only when size is zero. No allocation or global state. */
uint32_t mc100_crc32(const void *data, size_t size);
/* Encoders leave output unchanged on invalid input. */
mc100_result_t mc100_wav_header(uint8_t out[512], uint32_t pcm_bytes);
mc100_result_t mc100_index_encode(uint8_t out[64], const mc100_index_record_t *record);
mc100_result_t mc100_index_decode(const uint8_t in[64], mc100_index_record_t *record);
mc100_result_t mc100_index_header_encode(uint8_t out[512], const mc100_index_header_t *header);
mc100_result_t mc100_index_header_decode(const uint8_t in[512], mc100_index_header_t *header);
/* Initialize only from a valid CLAIMED header. Contradictions leave state unchanged. */
mc100_result_t mc100_index_validation_init(mc100_index_validation_t *validation,
                                            const mc100_index_header_t *header);
mc100_result_t mc100_index_validation_accept(mc100_index_validation_t *validation,
                                              const mc100_index_record_t *record);

#endif
