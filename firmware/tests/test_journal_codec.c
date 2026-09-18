#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mc100_format.h"

static void seal(uint8_t *bytes, size_t size)
{
    uint32_t crc = mc100_crc32(bytes, size - 4);
    for (unsigned i = 0; i < 4; ++i) bytes[size - 4 + i] = (uint8_t)(crc >> (i * 8));
}

static void assert_hex_fixture(const char *path, const uint8_t *bytes, size_t size)
{
    FILE *golden = fopen(path, "r");
    assert(golden != NULL);
    for (size_t i = 0; i < size; ++i) {
        unsigned byte = 256;
        assert(fscanf(golden, "%x", &byte) == 1);
        assert(byte <= 255 && bytes[i] == byte);
    }
    unsigned extra = 0;
    assert(fscanf(golden, "%x", &extra) == EOF);
    assert(fclose(golden) == 0);
}

static void test_record(const char *golden_path)
{
    mc100_index_record_t record = {
        .type = MC100_INDEX_BLOCK, .journal_seq = 3,
        .pcm_offset = 4096, .first_source_sample = 32000,
        .valid_bytes = 4096, .payload_crc32 = UINT32_C(0x12345678),
        .generation = 7
    };
    uint8_t bytes[64];
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    assert_hex_fixture(golden_path, bytes, sizeof(bytes));
    assert(memcmp(bytes, "MCR1\x01\x00\x01\x00", 8) == 0);
    assert(bytes[8] == 3 && bytes[16] == 0 && bytes[17] == 16);
    assert(bytes[24] == 0 && bytes[25] == 125);
    assert(bytes[32] == 0 && bytes[33] == 16);
    assert(memcmp(bytes + 36, "\x78\x56\x34\x12", 4) == 0);
    assert(bytes[40] == 7);
    for (size_t i = 48; i < 60; ++i) assert(bytes[i] == 0);
    mc100_index_record_t decoded = {0};
    assert(mc100_index_decode(bytes, &decoded) == MC100_OK);
    assert(decoded.generation == 7 && decoded.journal_seq == 3);
    assert(decoded.pcm_offset == 4096 && decoded.first_source_sample == 32000);
    assert(decoded.valid_bytes == 4096 && decoded.payload_crc32 == UINT32_C(0x12345678));
    for (size_t bit = 0; bit < 64 * 8; ++bit) {
        bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
        bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    record.valid_bytes = 4098;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.valid_bytes = 3;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.valid_bytes = 0;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.valid_bytes = 4096;
    record.pcm_offset = UINT64_MAX - 1;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.pcm_offset = 9600000;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.pcm_offset = 0;
    record.flags = 2;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.flags = 0;
    record.journal_seq = 4096;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.journal_seq = 0;
    record.generation = 0;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.generation = 7;
    record.type = MC100_INDEX_FINAL;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    record.pcm_offset = 9600000;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    assert(mc100_index_decode(bytes, &decoded) == MC100_OK);
    assert(decoded.type == MC100_INDEX_FINAL && decoded.pcm_offset == 9600000);
    record.type = MC100_INDEX_INCIDENT;
    record.flags = 1;
    record.detail = 5;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    assert(mc100_index_decode(bytes, &decoded) == MC100_OK);
    assert(decoded.flags == 1 && decoded.detail == 5);

    /* Recomputed CRC cannot make unknown schema/reserved fields acceptable. */
    bytes[4] = 2;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[56] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[48] = 2;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    record.type = MC100_INDEX_BLOCK;
    record.flags = 0;
    record.detail = 0;
    record.pcm_offset = 0;
    record.valid_bytes = 2;
    record.first_source_sample = UINT64_MAX;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    assert(mc100_index_encode(NULL, &record) == MC100_INVALID);
    assert(mc100_index_encode(bytes, NULL) == MC100_INVALID);
    assert(mc100_index_decode(NULL, &decoded) == MC100_INVALID);
    assert(mc100_index_decode(bytes, NULL) == MC100_INVALID);
}

static void test_header(const char *golden_path)
{
    uint8_t bytes[512];
    mc100_index_header_t header = {
        .flags = MC100_INDEX_CLAIMED, .generation = 7,
        .segment_index = 3, .first_source_sample = 32000
    };
    for (size_t i = 0; i < 16; ++i) header.boot_id[i] = (uint8_t)i;
    assert(mc100_index_header_encode(bytes, &header) == MC100_OK);
    assert_hex_fixture(golden_path, bytes, sizeof(bytes));
    assert(memcmp(bytes, "MC100IDX\x01\x00\x00\x02", 12) == 0);
    assert(bytes[12] == 2 && bytes[32] == 7 && bytes[40] == 3);
    assert(memcmp(bytes + 44, "\x80\x3e\x00\x00", 4) == 0);
    assert(memcmp(bytes + 48, "\x10\x00\x01\x00\x40\x01\x00\x00", 8) == 0);
    assert(memcmp(bytes + 64, "\x00\x7c\x92\x00\x40\x00\x00\x00\x00\x10\x00\x00", 12) == 0);
    for (size_t i = 76; i < 508; ++i) assert(bytes[i] == 0);
    mc100_index_header_t decoded = {0};
    assert(mc100_index_header_decode(bytes, &decoded) == MC100_OK);
    assert(decoded.generation == 7 && decoded.segment_index == 3);
    assert(decoded.first_source_sample == 32000);
    assert(memcmp(decoded.boot_id, header.boot_id, 16) == 0);
    for (size_t bit = 0; bit < 512 * 8; ++bit) {
        bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        assert(mc100_index_header_decode(bytes, &decoded) == MC100_CORRUPT);
        bytes[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    header.flags = MC100_INDEX_RESERVED;
    assert(mc100_index_header_encode(bytes, &header) == MC100_INVALID);
    header.generation = 0;
    header.segment_index = 0;
    header.first_source_sample = 0;
    assert(mc100_index_header_encode(bytes, &header) == MC100_OK);
    assert(mc100_index_header_decode(bytes, &decoded) == MC100_OK);
    assert(decoded.flags == MC100_INDEX_RESERVED && decoded.generation == 0);
    bytes[76] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_header_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(mc100_index_header_encode(bytes, &header) == MC100_OK);
    bytes[44] = 0;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_header_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(mc100_index_header_encode(bytes, &header) == MC100_OK);
    bytes[8] = 2;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_header_decode(bytes, &decoded) == MC100_CORRUPT);
    header.flags = MC100_INDEX_CLAIMED;
    header.generation = 1;
    header.first_source_sample = UINT64_MAX - 100;
    assert(mc100_index_header_encode(bytes, &header) == MC100_INVALID);
    header.flags = 3;
    assert(mc100_index_header_encode(bytes, &header) == MC100_INVALID);
    assert(mc100_index_header_encode(NULL, &header) == MC100_INVALID);
    assert(mc100_index_header_decode(NULL, &decoded) == MC100_INVALID);
}

static mc100_index_record_t block_record(uint64_t seq, uint64_t offset,
                                         uint64_t first_source_sample,
                                         uint32_t valid_bytes)
{
    mc100_index_record_t record = {
        .type = MC100_INDEX_BLOCK,
        .journal_seq = seq,
        .pcm_offset = offset,
        .first_source_sample = first_source_sample,
        .valid_bytes = valid_bytes,
        .payload_crc32 = UINT32_C(0x12345678),
        .generation = 7
    };
    return record;
}

static void assert_context_rejects_without_change(
    mc100_index_validation_t *validation, const mc100_index_record_t *record)
{
    const mc100_index_validation_t before = *validation;
    assert(mc100_index_validation_accept(validation, record) == MC100_CORRUPT);
    assert(memcmp(validation, &before, sizeof(before)) == 0);
}

static void test_context_validation(void)
{
    mc100_index_header_t header = {
        .flags = MC100_INDEX_CLAIMED,
        .generation = 7,
        .segment_index = 3,
        .first_source_sample = 1000
    };
    mc100_index_validation_t validation;
    memset(&validation, 0xa5, sizeof(validation));
    assert(mc100_index_validation_init(&validation, &header) == MC100_OK);

    uint8_t bytes[64];
    mc100_index_record_t parsed = {0};
    mc100_index_record_t record = block_record(0, 0, 1000, 4);
    mc100_index_validation_t trial = validation;
    record.journal_seq = 1;
    assert_context_rejects_without_change(&trial, &record);
    record.journal_seq = 0;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    assert(mc100_index_decode(bytes, &parsed) == MC100_OK);
    assert(mc100_index_validation_accept(&validation, &parsed) == MC100_OK);

    trial = validation;
    record = block_record(2, 4, 1002, 2);
    assert_context_rejects_without_change(&trial, &record);
    trial = validation;
    record = block_record(1, 4, 1002, 2);
    record.generation = 8;
    assert_context_rejects_without_change(&trial, &record);
    trial = validation;
    record = block_record(1, 2, 1001, 2);
    assert_context_rejects_without_change(&trial, &record);
    trial = validation;
    record = block_record(1, 4, 1003, 2);
    assert_context_rejects_without_change(&trial, &record);

    /* Decoding a checksummed record is necessary, not sufficient. */
    record = block_record(1, 4, 1002, 2);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[40] = 8;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &parsed) == MC100_OK);
    assert_context_rejects_without_change(&trial, &parsed);

    record = block_record(1, 4, 1002, 2);
    assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
    record.type = MC100_INDEX_CHECKPOINT;
    record.journal_seq = 2;
    record.pcm_offset = 6;
    record.first_source_sample = 1003;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    trial = validation;
    record.pcm_offset = 8;
    record.first_source_sample = 1004;
    assert_context_rejects_without_change(&trial, &record);
    record.pcm_offset = 6;
    record.first_source_sample = 1003;
    assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
    record.type = MC100_INDEX_FINAL;
    record.journal_seq = 3;
    assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
    record.type = MC100_INDEX_CHECKPOINT;
    record.journal_seq = 4;
    assert_context_rejects_without_change(&validation, &record);

    mc100_index_validation_t unchanged;
    memset(&unchanged, 0x5a, sizeof(unchanged));
    const mc100_index_validation_t initial = unchanged;
    header.flags = 3;
    assert(mc100_index_validation_init(&unchanged, &header) == MC100_CORRUPT);
    assert(memcmp(&unchanged, &initial, sizeof(initial)) == 0);
    assert(mc100_index_validation_init(NULL, &header) == MC100_INVALID);
    assert(mc100_index_validation_init(&unchanged, NULL) == MC100_INVALID);
    assert(mc100_index_validation_accept(NULL, &record) == MC100_INVALID);
    assert(mc100_index_validation_accept(&unchanged, NULL) == MC100_INVALID);

    header.flags = MC100_INDEX_CLAIMED;
    header.generation = 0;
    assert(mc100_index_validation_init(&unchanged, &header) == MC100_CORRUPT);
    header.generation = 7;
    header.first_source_sample = UINT64_MAX - 100;
    assert(mc100_index_validation_init(&unchanged, &header) == MC100_CORRUPT);
    assert(memcmp(&unchanged, &initial, sizeof(initial)) == 0);
    header.flags = MC100_INDEX_RESERVED;
    header.generation = 0;
    header.segment_index = 0;
    header.first_source_sample = 0;
    assert(mc100_index_validation_init(&unchanged, &header) == MC100_CORRUPT);
    assert(memcmp(&unchanged, &initial, sizeof(initial)) == 0);

    header.flags = MC100_INDEX_CLAIMED;
    header.generation = 7;
    header.segment_index = 3;
    header.first_source_sample = 1000;
    assert(mc100_index_validation_init(&validation, &header) == MC100_OK);
    record.type = MC100_INDEX_CHECKPOINT;
    record.pcm_offset = 0;
    record.first_source_sample = 1000;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    record.generation = 7;
    record.flags = 0;
    record.detail = 0;
    for (uint64_t seq = 0; seq < MC100_INDEX_MAX_RECORDS; ++seq) {
        record.journal_seq = seq;
        assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
    }
    record.journal_seq = MC100_INDEX_MAX_RECORDS;
    assert_context_rejects_without_change(&validation, &record);

    assert(mc100_index_validation_init(&validation, &header) == MC100_OK);
    record.type = MC100_INDEX_INCIDENT;
    record.journal_seq = 0;
    record.flags = MC100_INDEX_INCOMPLETE;
    record.detail = MC100_INCIDENT_MIC_IO;
    assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
    record.journal_seq = 1;
    assert_context_rejects_without_change(&validation, &record);

    /* Highest safe header sample base accepts the entire segment exactly. */
    header.first_source_sample = UINT64_MAX - UINT64_C(4800000);
    assert(mc100_index_validation_init(&validation, &header) == MC100_OK);
    uint64_t offset = 0;
    uint64_t seq = 0;
    while (offset < MC100_MAX_PCM_BYTES) {
        uint32_t size = (uint32_t)(MC100_MAX_PCM_BYTES - offset);
        if (size > MC100_PCM_BLOCK_BYTES) size = MC100_PCM_BLOCK_BYTES;
        record = block_record(seq++, offset, header.first_source_sample + offset / 2, size);
        assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
        offset += size;
    }
    record = block_record(seq, offset, UINT64_MAX, 2);
    assert_context_rejects_without_change(&validation, &record);
    record.type = MC100_INDEX_FINAL;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    assert(mc100_index_validation_accept(&validation, &record) == MC100_OK);
}

static void test_terminal_policy_with_recomputed_crc(void)
{
    uint8_t bytes[64];
    mc100_index_record_t decoded;
    mc100_index_record_t record = block_record(0, 0, 1000, 2);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);

    memset(&decoded, 0x5a, sizeof(decoded));
    const mc100_index_record_t unchanged = decoded;
    bytes[48] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);

    record.type = MC100_INDEX_CHECKPOINT;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[48] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);

    record.type = MC100_INDEX_FINAL;
    record.valid_bytes = 0;
    record.payload_crc32 = 0;
    record.flags = 0;
    record.detail = 0;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[52] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);

    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[48] = 1;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);

    record.type = MC100_INDEX_INCIDENT;
    record.flags = MC100_INDEX_INCOMPLETE;
    static const mc100_incident_reason_t reasons[] = {
        MC100_INCIDENT_MIC_IO, MC100_INCIDENT_STORAGE_IO,
        MC100_INCIDENT_STORAGE_FULL, MC100_INCIDENT_QUEUE_OVERFLOW,
        MC100_INCIDENT_ADC_INVALID, MC100_INCIDENT_CONFIG_INVALID,
        MC100_INCIDENT_RECOVERY_REQUIRED, MC100_INCIDENT_INTERNAL_PROTOCOL,
        MC100_INCIDENT_LOW_BAT_INTERRUPTED
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        record.detail = (uint32_t)reasons[i];
        assert(mc100_index_encode(bytes, &record) == MC100_OK);
        assert(bytes[52] == i + 1 && bytes[53] == 0 && bytes[54] == 0 && bytes[55] == 0);
    }
    for (uint32_t reason = (uint32_t)MC100_INCIDENT_MIC_IO;
         reason <= (uint32_t)MC100_INCIDENT_LOW_BAT_INTERRUPTED; ++reason) {
        record.detail = reason;
        assert(mc100_index_encode(bytes, &record) == MC100_OK);
        assert(mc100_index_decode(bytes, &decoded) == MC100_OK);
        assert(decoded.detail == reason);
    }
    record.detail = MC100_INCIDENT_LOW_BAT_INTERRUPTED + 1;
    assert(mc100_index_encode(bytes, &record) == MC100_INVALID);
    record.detail = MC100_INCIDENT_MIC_IO;
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[52] = 10;
    seal(bytes, sizeof(bytes));
    memset(&decoded, 0x5a, sizeof(decoded));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[52] = 0;
    seal(bytes, sizeof(bytes));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
    assert(mc100_index_encode(bytes, &record) == MC100_OK);
    bytes[48] = 0;
    seal(bytes, sizeof(bytes));
    memset(&decoded, 0x5a, sizeof(decoded));
    assert(mc100_index_decode(bytes, &decoded) == MC100_CORRUPT);
    assert(memcmp(&decoded, &unchanged, sizeof(decoded)) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    test_record(argv[1]);
    test_header(argv[2]);
    test_context_validation();
    test_terminal_policy_with_recomputed_crc();
    puts("journal_codec: golden layouts, record policy and contextual validation PASS");
    return 0;
}
