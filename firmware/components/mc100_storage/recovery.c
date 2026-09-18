#include "mc100_recovery.h"

#include "mc100_format.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { MC100_RECOVERY_ATTEMPTS = 64 };

typedef struct {
  const mc100_io_t *io;
  void *io_ctx;
  uint64_t deadline_ms;
  uint64_t (*now_ms)(void *);
  void *clock_ctx;
  mc100_recovery_report_t *report;
} recovery_t;

typedef struct {
  mc100_index_validation_t validation;
  uint32_t trusted_records;
  uint16_t terminal_type;
} scan_t;

typedef struct {
  char cursor[MC100_PATH_BYTES];
  char next[MC100_PATH_BYTES];
  bool found;
} next_candidate_t;

static mc100_result_t deadline(recovery_t *recovery) {
  if (recovery->now_ms(recovery->clock_ctx) < recovery->deadline_ms)
    return MC100_OK;
  ++recovery->report->timed_out;
  return MC100_TIMEOUT;
}

static bool ends_with(const char *value, const char *suffix) {
  size_t value_length = strlen(value);
  size_t suffix_length = strlen(suffix);
  return value_length >= suffix_length &&
         !strcmp(value + value_length - suffix_length, suffix);
}

static bool normal_base(const char *value) {
  size_t length = strlen(value);
  if (length < 36 || value[32] != '_')
    return false;
  for (size_t i = 0; i < 32; ++i)
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f')))
      return false;
  const char *cursor = value + 33;
  if (*cursor < '1' || *cursor > '9')
    return false;
  while (*cursor >= '0' && *cursor <= '9')
    ++cursor;
  if (*cursor++ != '_' || *cursor < '0' || *cursor > '9')
    return false;
  while (*cursor >= '0' && *cursor <= '9')
    ++cursor;
  return *cursor == 0;
}

static bool candidate_base(const char *path, char out[MC100_PATH_BYTES]) {
  static const char *const suffixes[] = {".partial.wav", ".wav.part", ".idx.part",
                                         ".wav", ".idx"};
  if (ends_with(path, ".recovered.wav") || strstr(path, ".recovering_") != NULL)
    return false;
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
    if (!ends_with(path, suffixes[i]))
      continue;
    size_t length = strlen(path) - strlen(suffixes[i]);
    if (length >= MC100_PATH_BYTES)
      return false;
    memcpy(out, path, length);
    out[length] = 0;
    return normal_base(out);
  }
  return false;
}

static mc100_result_t next_visit(void *ctx, const char *path) {
  next_candidate_t *candidate = ctx;
  char base[MC100_PATH_BYTES];
  if (!candidate_base(path, base) || strcmp(base, candidate->cursor) <= 0)
    return MC100_OK;
  if (!candidate->found || strcmp(base, candidate->next) < 0) {
    memcpy(candidate->next, base, strlen(base) + 1);
    candidate->found = true;
  }
  return MC100_OK;
}

static mc100_result_t next_base(recovery_t *recovery, const char *cursor,
                                char next[MC100_PATH_BYTES], bool *found) {
  next_candidate_t candidate;
  memset(&candidate, 0, sizeof(candidate));
  memcpy(candidate.cursor, cursor, strlen(cursor) + 1);
  mc100_result_t result = deadline(recovery);
  if (result != MC100_OK)
    return result;
  result = recovery->io->list(recovery->io_ctx, next_visit, &candidate);
  if (result != MC100_OK)
    return result;
  *found = candidate.found;
  if (candidate.found)
    memcpy(next, candidate.next, strlen(candidate.next) + 1);
  return MC100_OK;
}

static bool make_path(char out[MC100_PATH_BYTES], const char *base,
                      const char *suffix) {
  int length = snprintf(out, MC100_PATH_BYTES, "%s%s", base, suffix);
  return length > 0 && length < MC100_PATH_BYTES;
}

static mc100_result_t path_exists(recovery_t *recovery, const char *path,
                                  bool *exists, uint64_t *size) {
  mc100_result_t result = deadline(recovery);
  if (result != MC100_OK)
    return result;
  uint64_t local_size = 0;
  result = recovery->io->stat(recovery->io_ctx, path, &local_size);
  if (result == MC100_NOT_READY) {
    *exists = false;
    if (size != NULL)
      *size = 0;
    return MC100_OK;
  }
  if (result != MC100_OK)
    return result;
  *exists = true;
  if (size != NULL)
    *size = local_size;
  return MC100_OK;
}

static mc100_result_t read_some(recovery_t *recovery, mc100_file_t file,
                                uint64_t offset, void *data, size_t size,
                                size_t *actual) {
  mc100_result_t result = deadline(recovery);
  if (result != MC100_OK)
    return result;
  return recovery->io->read_at(recovery->io_ctx, file, offset, data, size,
                               actual);
}

static mc100_result_t read_exact(recovery_t *recovery, mc100_file_t file,
                                 uint64_t offset, void *data, size_t size) {
  size_t actual = 0;
  mc100_result_t result =
      read_some(recovery, file, offset, data, size, &actual);
  if (result != MC100_OK)
    return result;
  return actual == size ? MC100_OK : MC100_CORRUPT;
}

static mc100_result_t write_exact(recovery_t *recovery, mc100_file_t file,
                                  uint64_t offset, const void *data,
                                  size_t size) {
  mc100_result_t result = deadline(recovery);
  if (result != MC100_OK)
    return result;
  size_t actual = 0;
  result = recovery->io->write_at(recovery->io_ctx, file, offset, data, size,
                                  &actual);
  if (result != MC100_OK)
    return result;
  return actual == size ? MC100_OK : MC100_IO;
}

static void boot_text(char out[33], const uint8_t boot[16]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    out[i * 2] = hex[boot[i] >> 4];
    out[i * 2 + 1] = hex[boot[i] & 15];
  }
  out[32] = 0;
}

static bool header_matches_base(const mc100_index_header_t *header,
                                const char *base) {
  char boot[33];
  char expected[MC100_PATH_BYTES];
  boot_text(boot, header->boot_id);
  int length = snprintf(expected, sizeof(expected), "%s_%" PRIu64 "_%" PRIu32,
                        boot, header->generation, header->segment_index);
  return length > 0 && length < (int)sizeof(expected) && !strcmp(expected, base);
}

static mc100_result_t scan_index(recovery_t *recovery, mc100_file_t index,
                                 uint64_t index_size, mc100_file_t wav,
                                 const mc100_index_header_t *header,
                                 scan_t *scan) {
  memset(scan, 0, sizeof(*scan));
  mc100_result_t result =
      mc100_index_validation_init(&scan->validation, header);
  if (result != MC100_OK)
    return MC100_CORRUPT;
  uint8_t record_bytes[MC100_INDEX_RECORD_BYTES];
  uint8_t pcm[MC100_PCM_BLOCK_BYTES];
  for (uint32_t i = 0; i < MC100_INDEX_MAX_RECORDS; ++i) {
    uint64_t offset = MC100_INDEX_HEADER_BYTES +
                      (uint64_t)i * MC100_INDEX_RECORD_BYTES;
    if (offset > index_size || index_size - offset < MC100_INDEX_RECORD_BYTES)
      break;
    size_t actual = 0;
    result = read_some(recovery, index, offset, record_bytes,
                       sizeof(record_bytes), &actual);
    if (result != MC100_OK)
      return result;
    if (actual != sizeof(record_bytes))
      break;
    mc100_index_record_t record = {0};
    if (mc100_index_decode(record_bytes, &record) != MC100_OK)
      break;
    mc100_index_validation_t next = scan->validation;
    if (mc100_index_validation_accept(&next, &record) != MC100_OK)
      break;
    if (record.type == MC100_INDEX_BLOCK) {
      result = read_some(recovery, wav,
                         MC100_WAV_HEADER_BYTES + record.pcm_offset, pcm,
                         record.valid_bytes, &actual);
      if (result != MC100_OK)
        return result;
      if (actual != record.valid_bytes ||
          mc100_crc32(pcm, record.valid_bytes) != record.payload_crc32)
        break;
    }
    scan->validation = next;
    scan->trusted_records = i + 1;
    if (record.type == MC100_INDEX_FINAL ||
        record.type == MC100_INDEX_INCIDENT) {
      scan->terminal_type = record.type;
      break;
    }
  }
  return MC100_OK;
}

static mc100_result_t verify_recovered(recovery_t *recovery,
                                       const char *path,
                                       mc100_file_t index,
                                       const mc100_index_header_t *header,
                                       const scan_t *trusted) {
  bool exists = false;
  uint64_t size = 0;
  mc100_result_t result = path_exists(recovery, path, &exists, &size);
  if (result != MC100_OK || !exists)
    return result != MC100_OK ? result : MC100_NOT_READY;
  if (size != MC100_WAV_HEADER_BYTES + trusted->validation.pcm_bytes)
    return MC100_CORRUPT;
  mc100_file_t file = NULL;
  result = recovery->io->open_read(recovery->io_ctx, path, &file);
  if (result != MC100_OK)
    return result;
  uint8_t actual_header[MC100_WAV_HEADER_BYTES];
  uint8_t expected_header[MC100_WAV_HEADER_BYTES];
  result = read_exact(recovery, file, 0, actual_header, sizeof(actual_header));
  if (result == MC100_OK)
    result = mc100_wav_header(expected_header,
                              (uint32_t)trusted->validation.pcm_bytes);
  if (result == MC100_OK &&
      memcmp(actual_header, expected_header, sizeof(actual_header)))
    result = MC100_CORRUPT;
  mc100_index_validation_t validation = {0};
  if (result == MC100_OK)
    result = mc100_index_validation_init(&validation, header);
  uint8_t encoded[MC100_INDEX_RECORD_BYTES];
  uint8_t pcm[MC100_PCM_BLOCK_BYTES];
  for (uint32_t i = 0; result == MC100_OK && i < trusted->trusted_records;
       ++i) {
    result = read_exact(recovery, index,
                        MC100_INDEX_HEADER_BYTES +
                            (uint64_t)i * MC100_INDEX_RECORD_BYTES,
                        encoded, sizeof(encoded));
    mc100_index_record_t record = {0};
    if (result == MC100_OK)
      result = mc100_index_decode(encoded, &record);
    if (result == MC100_OK)
      result = mc100_index_validation_accept(&validation, &record);
    if (result == MC100_OK && record.type == MC100_INDEX_BLOCK) {
      result = read_exact(recovery, file,
                          MC100_WAV_HEADER_BYTES + record.pcm_offset, pcm,
                          record.valid_bytes);
      if (result == MC100_OK &&
          mc100_crc32(pcm, record.valid_bytes) != record.payload_crc32)
        result = MC100_CORRUPT;
    }
  }
  if (result == MC100_OK &&
      validation.pcm_bytes != trusted->validation.pcm_bytes)
    result = MC100_CORRUPT;
  mc100_result_t closed = recovery->io->close(recovery->io_ctx, file);
  return result != MC100_OK ? result : closed;
}

static mc100_result_t create_recovered(recovery_t *recovery, const char *base,
                                       const char *destination,
                                       mc100_file_t source,
                                       uint64_t pcm_bytes) {
  uint64_t total = 0;
  uint64_t free_bytes = 0;
  mc100_result_t result = deadline(recovery);
  if (result != MC100_OK)
    return result;
  result = recovery->io->space(recovery->io_ctx, &total, &free_bytes);
  if (result != MC100_OK)
    return result;
  if (total == 0 || free_bytes > total ||
      pcm_bytes > UINT64_MAX - MC100_WAV_HEADER_BYTES ||
      free_bytes < MC100_WAV_HEADER_BYTES + pcm_bytes)
    return MC100_FULL;

  mc100_file_t output = NULL;
  char temporary[MC100_PATH_BYTES];
  for (uint32_t attempt = 0; attempt < MC100_RECOVERY_ATTEMPTS; ++attempt) {
    char suffix[32];
    int suffix_length =
        snprintf(suffix, sizeof(suffix), ".recovering_%" PRIu32 ".wav.part",
                 attempt);
    if (suffix_length <= 0 || suffix_length >= (int)sizeof(suffix) ||
        !make_path(temporary, base, suffix))
      return MC100_INVALID;
    result = recovery->io->open_exclusive(recovery->io_ctx, temporary, &output);
    if (result == MC100_NOT_READY)
      continue;
    if (result != MC100_OK)
      return result;
    break;
  }
  if (output == NULL)
    return MC100_NOT_READY;

  uint8_t buffer[MC100_PCM_BLOCK_BYTES];
  result = mc100_wav_header(buffer, (uint32_t)pcm_bytes);
  if (result == MC100_OK)
    result = write_exact(recovery, output, 0, buffer, MC100_WAV_HEADER_BYTES);
  uint64_t copied = 0;
  while (result == MC100_OK && copied < pcm_bytes) {
    size_t chunk = (size_t)(pcm_bytes - copied);
    if (chunk > sizeof(buffer))
      chunk = sizeof(buffer);
    result = read_exact(recovery, source, MC100_WAV_HEADER_BYTES + copied,
                        buffer, chunk);
    if (result == MC100_OK)
      result = write_exact(recovery, output, MC100_WAV_HEADER_BYTES + copied,
                           buffer, chunk);
    copied += result == MC100_OK ? chunk : 0;
  }
  if (result == MC100_OK) {
    result = deadline(recovery);
    if (result == MC100_OK)
      result = recovery->io->sync(recovery->io_ctx, output);
  }
  if (result == MC100_OK)
    result = recovery->io->close(recovery->io_ctx, output);
  else
    (void)recovery->io->close(recovery->io_ctx, output);
  if (result != MC100_OK)
    return result;
  return recovery->io->rename_no_replace(recovery->io_ctx, temporary,
                                         destination);
}

static void report_invalid(mc100_recovery_report_t *report) {
  ++report->invalid;
  ++report->preserved;
}

static mc100_result_t process_base(recovery_t *recovery, const char *base) {
  char idx_final[MC100_PATH_BYTES], idx_part[MC100_PATH_BYTES];
  char wav_final[MC100_PATH_BYTES], wav_part[MC100_PATH_BYTES];
  char wav_partial[MC100_PATH_BYTES], recovered[MC100_PATH_BYTES];
  if (!make_path(idx_final, base, ".idx") ||
      !make_path(idx_part, base, ".idx.part") ||
      !make_path(wav_final, base, ".wav") ||
      !make_path(wav_part, base, ".wav.part") ||
      !make_path(wav_partial, base, ".partial.wav") ||
      !make_path(recovered, base, ".recovered.wav"))
    return MC100_INVALID;

  bool has_idx_final = false, has_idx_part = false;
  bool has_wav_final = false, has_wav_part = false, has_wav_partial = false;
  uint64_t idx_final_size = 0, idx_part_size = 0;
  mc100_result_t result = path_exists(recovery, idx_final, &has_idx_final,
                                      &idx_final_size);
  if (result == MC100_OK)
    result = path_exists(recovery, idx_part, &has_idx_part, &idx_part_size);
  if (result == MC100_OK)
    result = path_exists(recovery, wav_final, &has_wav_final, NULL);
  if (result == MC100_OK)
    result = path_exists(recovery, wav_part, &has_wav_part, NULL);
  if (result == MC100_OK)
    result = path_exists(recovery, wav_partial, &has_wav_partial, NULL);
  if (result != MC100_OK)
    return result;
  unsigned index_count = (unsigned)has_idx_final + (unsigned)has_idx_part;
  unsigned wav_count = (unsigned)has_wav_final + (unsigned)has_wav_part +
                       (unsigned)has_wav_partial;
  if (index_count != 1 || wav_count != 1) {
    report_invalid(recovery->report);
    return MC100_OK;
  }
  const char *idx_path = has_idx_final ? idx_final : idx_part;
  uint64_t idx_size = has_idx_final ? idx_final_size : idx_part_size;
  const char *wav_path =
      has_wav_final ? wav_final : has_wav_part ? wav_part : wav_partial;
  if (idx_size < MC100_INDEX_HEADER_BYTES) {
    report_invalid(recovery->report);
    return MC100_OK;
  }

  mc100_file_t index = NULL;
  mc100_file_t wav = NULL;
  result = recovery->io->open_read(recovery->io_ctx, idx_path, &index);
  if (result == MC100_OK)
    result = recovery->io->open_read(recovery->io_ctx, wav_path, &wav);
  uint8_t encoded_header[MC100_INDEX_HEADER_BYTES];
  mc100_index_header_t header;
  if (result == MC100_OK)
    result = read_exact(recovery, index, 0, encoded_header,
                        sizeof(encoded_header));
  if (result == MC100_OK)
    result = mc100_index_header_decode(encoded_header, &header);
  if (result == MC100_OK && !header_matches_base(&header, base))
    result = MC100_CORRUPT;
  scan_t scan = {0};
  if (result == MC100_OK)
    result = scan_index(recovery, index, idx_size, wav, &header, &scan);
  if (result == MC100_OK && scan.validation.pcm_bytes == 0)
    result = MC100_CORRUPT;

  if (result == MC100_CORRUPT) {
    report_invalid(recovery->report);
    result = MC100_OK;
  } else if (result == MC100_OK) {
    mc100_result_t verified =
        verify_recovered(recovery, recovered, index, &header, &scan);
    if (verified == MC100_OK) {
      ++recovery->report->preserved;
    } else if (verified == MC100_NOT_READY) {
      result = create_recovered(recovery, base, recovered, wav,
                                scan.validation.pcm_bytes);
      if (result == MC100_OK) {
        ++recovery->report->recovered;
        recovery->report->valid_pcm_bytes += scan.validation.pcm_bytes;
      } else {
        ++recovery->report->preserved;
      }
    } else if (verified == MC100_CORRUPT) {
      report_invalid(recovery->report);
    } else {
      result = verified;
    }
  }
  if (wav != NULL) {
    mc100_result_t closed = recovery->io->close(recovery->io_ctx, wav);
    if (result == MC100_OK)
      result = closed;
  }
  if (index != NULL) {
    mc100_result_t closed = recovery->io->close(recovery->io_ctx, index);
    if (result == MC100_OK)
      result = closed;
  }
  return result;
}

mc100_result_t mc100_recover(const mc100_io_t *io, void *ctx,
                             uint64_t deadline_ms,
                             uint64_t (*now_ms)(void *), void *clock_ctx,
                             mc100_recovery_report_t *report) {
  if (io == NULL || now_ms == NULL || report == NULL ||
      io->open_exclusive == NULL || io->open_read == NULL ||
      io->read_at == NULL || io->write_at == NULL || io->sync == NULL ||
      io->close == NULL || io->rename_no_replace == NULL ||
      io->stat == NULL || io->space == NULL || io->list == NULL)
    return MC100_INVALID;
  memset(report, 0, sizeof(*report));
  recovery_t recovery = {.io = io,
                         .io_ctx = ctx,
                         .deadline_ms = deadline_ms,
                         .now_ms = now_ms,
                         .clock_ctx = clock_ctx,
                         .report = report};
  char cursor[MC100_PATH_BYTES] = {0};
  for (;;) {
    char base[MC100_PATH_BYTES];
    bool found = false;
    mc100_result_t result = next_base(&recovery, cursor, base, &found);
    if (result != MC100_OK || !found)
      return result;
    memcpy(cursor, base, strlen(base) + 1);
    result = deadline(&recovery);
    if (result == MC100_OK)
      result = process_base(&recovery, base);
    if (result != MC100_OK)
      return result;
  }
}
