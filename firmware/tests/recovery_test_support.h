#ifndef MC100_RECOVERY_TEST_SUPPORT_H
#define MC100_RECOVERY_TEST_SUPPORT_H

#include "mc100_fake_io.h"
#include "mc100_format.h"
#include "mc100_recovery.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t recovery_test_now(void *ctx) {
  return *(const uint64_t *)ctx;
}

static void recovery_test_base(char out[MC100_PATH_BYTES],
                               const uint8_t boot[16], uint64_t generation,
                               uint32_t segment) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    out[i * 2] = hex[boot[i] >> 4];
    out[i * 2 + 1] = hex[boot[i] & 15];
  }
  int length = snprintf(out + 32, MC100_PATH_BYTES - 32, "_%" PRIu64 "_%" PRIu32,
                        generation, segment);
  assert(length > 0 && (size_t)length < MC100_PATH_BYTES - 32);
}

static void recovery_test_path(char out[MC100_PATH_BYTES], const char *base,
                               const char *suffix) {
  int length = snprintf(out, MC100_PATH_BYTES, "%s%s", base, suffix);
  assert(length > 0 && length < MC100_PATH_BYTES);
}

static mc100_file_t recovery_test_create(mc100_fake_io_t *fake,
                                         const char *path, uint64_t size) {
  const mc100_io_t *io = mc100_fake_io_ops();
  mc100_file_t file = NULL;
  assert(io->open_exclusive(fake, path, &file) == MC100_OK);
  uint64_t actual = 0;
  assert(io->allocate(fake, file, size, &actual) == MC100_OK);
  assert(actual == size);
  return file;
}

static void recovery_test_write(mc100_fake_io_t *fake, mc100_file_t file,
                                uint64_t offset, const void *data,
                                size_t size) {
  size_t actual = 0;
  assert(mc100_fake_io_ops()->write_at(fake, file, offset, data, size,
                                       &actual) == MC100_OK);
  assert(actual == size);
}

static void recovery_test_finish(mc100_fake_io_t *fake, mc100_file_t file) {
  const mc100_io_t *io = mc100_fake_io_ops();
  assert(io->sync(fake, file) == MC100_OK);
  assert(io->close(fake, file) == MC100_OK);
}

static void recovery_test_wav(mc100_fake_io_t *fake, const char *path,
                              const uint8_t *pcm, size_t pcm_bytes,
                              size_t allocated_pcm, uint32_t header_pcm) {
  assert(pcm_bytes <= allocated_pcm);
  mc100_file_t file =
      recovery_test_create(fake, path, MC100_WAV_HEADER_BYTES + allocated_pcm);
  uint8_t header[MC100_WAV_HEADER_BYTES];
  assert(mc100_wav_header(header, header_pcm) == MC100_OK);
  recovery_test_write(fake, file, 0, header, sizeof(header));
  recovery_test_write(fake, file, MC100_WAV_HEADER_BYTES, pcm, pcm_bytes);
  recovery_test_finish(fake, file);
}

static void recovery_test_idx(mc100_fake_io_t *fake, const char *path,
                              const mc100_index_header_t *header,
                              const mc100_index_record_t *records,
                              size_t record_count, size_t allocated_tail) {
  assert(record_count <= MC100_INDEX_MAX_RECORDS);
  size_t used = MC100_INDEX_HEADER_BYTES +
                record_count * MC100_INDEX_RECORD_BYTES;
  assert(allocated_tail <= MC100_INDEX_MAX_RECORDS * MC100_INDEX_RECORD_BYTES);
  assert(used <= MC100_INDEX_HEADER_BYTES + allocated_tail);
  mc100_file_t file = recovery_test_create(
      fake, path, MC100_INDEX_HEADER_BYTES + allocated_tail);
  uint8_t encoded_header[MC100_INDEX_HEADER_BYTES];
  assert(mc100_index_header_encode(encoded_header, header) == MC100_OK);
  recovery_test_write(fake, file, 0, encoded_header, sizeof(encoded_header));
  for (size_t i = 0; i < record_count; ++i) {
    uint8_t encoded[MC100_INDEX_RECORD_BYTES];
    assert(mc100_index_encode(encoded, &records[i]) == MC100_OK);
    recovery_test_write(fake, file,
                        MC100_INDEX_HEADER_BYTES +
                            i * MC100_INDEX_RECORD_BYTES,
                        encoded, sizeof(encoded));
  }
  recovery_test_finish(fake, file);
}

static bool recovery_test_exists(const mc100_fake_io_t *fake,
                                 const char *path) {
  size_t size = 0;
  return mc100_fake_io_bytes(fake, path, &size) != NULL;
}

static uint32_t recovery_test_hash(const mc100_fake_io_t *fake,
                                   const char *path) {
  size_t size = 0;
  const uint8_t *bytes = mc100_fake_io_bytes(fake, path, &size);
  assert(bytes != NULL);
  return mc100_crc32(bytes, size);
}

#endif
