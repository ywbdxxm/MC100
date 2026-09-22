#include "recovery_test_support.h"

#include <limits.h>

static mc100_index_header_t claimed_header(const uint8_t boot[16],
                                           uint64_t generation,
                                           uint32_t segment) {
  mc100_index_header_t header = {0};
  header.flags = MC100_INDEX_CLAIMED;
  memcpy(header.boot_id, boot, 16);
  header.generation = generation;
  header.segment_index = segment;
  header.first_source_sample = 32000;
  return header;
}

static mc100_index_record_t block_record(const mc100_index_header_t *header,
                                         uint64_t sequence, uint64_t offset,
                                         const uint8_t *pcm, uint32_t bytes) {
  mc100_index_record_t record = {0};
  record.type = MC100_INDEX_BLOCK;
  record.journal_seq = sequence;
  record.pcm_offset = offset;
  record.first_source_sample = header->first_source_sample + offset / 2;
  record.valid_bytes = bytes;
  record.payload_crc32 = mc100_crc32(pcm, bytes);
  record.generation = header->generation;
  return record;
}

static void bad_block_stops_the_prefix(void) {
  const uint8_t boot[16] = {0x11};
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  recovery_test_base(base, boot, 7, 0);
  recovery_test_path(wav, base, ".wav.part");
  recovery_test_path(idx, base, ".idx.part");
  recovery_test_path(recovered, base, ".recovered.wav");

  uint8_t pcm[3 * MC100_PCM_BLOCK_BYTES];
  for (size_t i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(i * 37u + 11u);
  mc100_index_header_t header = claimed_header(boot, 7, 0);
  mc100_index_record_t records[3];
  for (size_t i = 0; i < 3; ++i)
    records[i] = block_record(&header, i, i * MC100_PCM_BLOCK_BYTES,
                              pcm + i * MC100_PCM_BLOCK_BYTES,
                              MC100_PCM_BLOCK_BYTES);
  records[1].payload_crc32 ^= UINT32_C(0x01000000);

  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  recovery_test_wav(fake, wav, pcm, sizeof(pcm), sizeof(pcm) + 8192, 0);
  recovery_test_idx(fake, idx, &header, records, 3, 8 * MC100_INDEX_RECORD_BYTES);
  uint32_t wav_hash = recovery_test_hash(fake, wav);
  uint32_t idx_hash = recovery_test_hash(fake, idx);

  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 1 && report.valid_pcm_bytes == 4096);
  assert(report.timed_out == 0);
  assert(recovery_test_hash(fake, wav) == wav_hash);
  assert(recovery_test_hash(fake, idx) == idx_hash);

  size_t size = 0;
  const uint8_t *bytes = mc100_fake_io_bytes(fake, recovered, &size);
  assert(bytes != NULL && size == MC100_WAV_HEADER_BYTES + 4096);
  uint8_t expected_header[MC100_WAV_HEADER_BYTES];
  assert(mc100_wav_header(expected_header, 4096) == MC100_OK);
  assert(memcmp(bytes, expected_header, sizeof(expected_header)) == 0);
  assert(memcmp(bytes + MC100_WAV_HEADER_BYTES, pcm, 4096) == 0);
  mc100_fake_io_destroy(fake);
}

static void torn_index_tail_keeps_the_last_complete_block(void) {
  const uint8_t boot[16] = {0x22};
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  recovery_test_base(base, boot, 9, 3);
  recovery_test_path(wav, base, ".wav.part");
  recovery_test_path(idx, base, ".idx.part");
  recovery_test_path(recovered, base, ".recovered.wav");
  uint8_t pcm[640];
  for (size_t i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(i ^ 0x5a);
  mc100_index_header_t header = claimed_header(boot, 9, 3);
  mc100_index_record_t record = block_record(&header, 0, 0, pcm, sizeof(pcm));

  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  recovery_test_wav(fake, wav, pcm, sizeof(pcm), 4096, 0);
  recovery_test_idx(fake, idx, &header, &record, 1,
                    MC100_INDEX_RECORD_BYTES + 13);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 1 && report.valid_pcm_bytes == sizeof(pcm));
  size_t size = 0;
  assert(mc100_fake_io_bytes(fake, recovered, &size) != NULL);
  assert(size == MC100_WAV_HEADER_BYTES + sizeof(pcm));
  mc100_fake_io_destroy(fake);
}

static uint32_t fuzz_next(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void put_u32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = (uint8_t)(value >> (i * 8));
}

static void metadata_fuzz_10000(void) {
  const uint8_t boot[16] = {0x33};
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  recovery_test_base(base, boot, 5, 1);
  recovery_test_path(wav, base, ".wav.part");
  recovery_test_path(idx, base, ".idx.part");
  recovery_test_path(recovered, base, ".recovered.wav");
  mc100_index_header_t header = claimed_header(boot, 5, 1);
  uint8_t pcm[4 * 64 + MC100_PCM_BLOCK_BYTES];
  for (size_t i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(i * 29u + 7u);

  uint32_t seed = UINT32_C(0x6d633130);
  for (size_t case_index = 0; case_index < 10000; ++case_index) {
    size_t prefix_count = 1 + fuzz_next(&seed) % 4;
    size_t prefix_bytes = prefix_count * 64;
    unsigned field = (unsigned)(case_index % 3);
    uint32_t varied_length =
        2 + 2 * (fuzz_next(&seed) % (MC100_PCM_BLOCK_BYTES / 2));
    size_t source_bytes =
        prefix_bytes + (field == 1 ? varied_length : 64);
    size_t expected_bytes =
        prefix_bytes + (field == 1 ? varied_length : 0);
    mc100_index_record_t records[5];
    for (size_t i = 0; i < prefix_count; ++i)
      records[i] = block_record(&header, i, i * 64, pcm + i * 64, 64);
    records[prefix_count] =
        block_record(&header, prefix_count, prefix_bytes, pcm + prefix_bytes,
                     field == 1 ? varied_length : 64);
    if (field == 0) {
      uint64_t offset_delta = 2 + 2 * (fuzz_next(&seed) % 32);
      records[prefix_count].pcm_offset += offset_delta;
      records[prefix_count].first_source_sample += offset_delta / 2;
    } else if (field == 2) {
      records[prefix_count].journal_seq += 1 + fuzz_next(&seed) % 16;
    }

    mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(1048576));
    assert(fake != NULL);
    recovery_test_wav(fake, wav, pcm, source_bytes, source_bytes, 0);
    size_t record_count = prefix_count + 1;
    size_t tail = fuzz_next(&seed) % MC100_INDEX_RECORD_BYTES;
    recovery_test_idx(fake, idx, &header, records, record_count,
                      record_count * MC100_INDEX_RECORD_BYTES + tail);
    uint64_t now = 0;
    mc100_recovery_report_t report;
    mc100_result_t result = mc100_recover(mc100_fake_io_ops(), fake, 30000,
                                          recovery_test_now, &now, &report);
    assert(result == MC100_OK);
    assert(report.recovered == 1 && report.timed_out == 0);
    assert(report.valid_pcm_bytes == expected_bytes);
    size_t recovered_size = 0;
    const uint8_t *recovered_bytes =
        mc100_fake_io_bytes(fake, recovered, &recovered_size);
    assert(recovered_bytes != NULL);
    assert(recovered_size == MC100_WAV_HEADER_BYTES + expected_bytes);
    assert(memcmp(recovered_bytes + MC100_WAV_HEADER_BYTES, pcm,
                  expected_bytes) == 0);
    mc100_fake_io_destroy(fake);
  }
}

static void invalid_inputs_are_reported_and_preserved(void) {
  const uint8_t boot[16] = {0x55};
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  recovery_test_base(base, boot, 6, 2);
  recovery_test_path(wav, base, ".wav.part");
  recovery_test_path(idx, base, ".idx.part");
  recovery_test_path(recovered, base, ".recovered.wav");
  uint8_t pcm[64];
  memset(pcm, 0x39, sizeof(pcm));

  mc100_fake_io_t *missing = mc100_fake_io_create(UINT64_C(1048576));
  assert(missing != NULL);
  recovery_test_wav(missing, wav, pcm, sizeof(pcm), sizeof(pcm), 0);
  uint32_t source_hash = recovery_test_hash(missing, wav);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), missing, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 &&
         report.invalid == 1);
  assert(!recovery_test_exists(missing, recovered));
  assert(recovery_test_hash(missing, wav) == source_hash);
  mc100_fake_io_destroy(missing);

  mc100_fake_io_t *unknown = mc100_fake_io_create(UINT64_C(1048576));
  assert(unknown != NULL);
  recovery_test_wav(unknown, wav, pcm, sizeof(pcm), sizeof(pcm), 0);
  mc100_index_header_t header = claimed_header(boot, 6, 2);
  uint8_t bytes[MC100_INDEX_HEADER_BYTES];
  assert(mc100_index_header_encode(bytes, &header) == MC100_OK);
  bytes[8] = 2;
  put_u32(bytes + 508, mc100_crc32(bytes, 508));
  mc100_file_t file = recovery_test_create(unknown, idx, sizeof(bytes));
  recovery_test_write(unknown, file, 0, bytes, sizeof(bytes));
  recovery_test_finish(unknown, file);
  source_hash = recovery_test_hash(unknown, idx);
  assert(mc100_recover(mc100_fake_io_ops(), unknown, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 &&
         report.invalid == 1);
  assert(recovery_test_hash(unknown, idx) == source_hash);
  assert(!recovery_test_exists(unknown, recovered));
  mc100_fake_io_destroy(unknown);
}

int main(void) {
  bad_block_stops_the_prefix();
  torn_index_tail_keeps_the_last_complete_block();
  invalid_inputs_are_reported_and_preserved();
  metadata_fuzz_10000();
  puts("recover_prefix: first-bad-block stop, torn tail, originals preserved, "
       "10000 valid-prefix offset/length/count cases PASS");
  return 0;
}
