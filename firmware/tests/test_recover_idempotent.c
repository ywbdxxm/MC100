#include "recovery_test_support.h"
#include "mc100_writer.h"

#include <limits.h>

typedef struct {
  uint64_t now;
  uint64_t step;
} advancing_clock_t;

static uint64_t advancing_now(void *ctx) {
  advancing_clock_t *clock = ctx;
  uint64_t value = clock->now;
  clock->now += clock->step;
  return value;
}

static uint64_t *list_clock;
static size_t list_visits;
static bool advance_after_list;

typedef struct {
  mc100_io_visit_fn visit;
  void *ctx;
} timed_visitor_t;

static mc100_result_t timed_visit(void *ctx, const char *path) {
  timed_visitor_t *visitor = ctx;
  ++list_visits;
  ++*list_clock;
  return visitor->visit(visitor->ctx, path);
}

static mc100_result_t timed_list(void *ctx, mc100_io_visit_fn visit,
                                 void *visit_ctx) {
  timed_visitor_t visitor = {visit, visit_ctx};
  mc100_result_t result =
      mc100_fake_io_ops()->list(ctx, timed_visit, &visitor);
  if (advance_after_list)
    ++*list_clock;
  return result;
}

static uint64_t *publication_clock;
static size_t faulting_close_calls;

static mc100_result_t timed_sync(void *ctx, mc100_file_t file) {
  mc100_result_t result = mc100_fake_io_ops()->sync(ctx, file);
  if (result == MC100_OK)
    ++*publication_clock;
  return result;
}

static mc100_result_t timed_close(void *ctx, mc100_file_t file) {
  mc100_result_t result = mc100_fake_io_ops()->close(ctx, file);
  if (result == MC100_OK)
    ++*publication_clock;
  return result;
}

static mc100_result_t fail_both_closes(void *ctx, mc100_file_t file) {
  assert(mc100_fake_io_ops()->close(ctx, file) == MC100_OK);
  ++faulting_close_calls;
  return faulting_close_calls == 1 ? MC100_IO : MC100_FULL;
}

static mc100_result_t timed_rename(void *ctx, const char *source,
                                   const char *destination) {
  mc100_result_t result =
      mc100_fake_io_ops()->rename_no_replace(ctx, source, destination);
  if (result == MC100_OK)
    ++*publication_clock;
  return result;
}

static void create_candidate(mc100_fake_io_t *fake, char base[MC100_PATH_BYTES],
                             char wav[MC100_PATH_BYTES],
                             char idx[MC100_PATH_BYTES]) {
  const uint8_t boot[16] = {0x44};
  recovery_test_base(base, boot, 12, 0);
  recovery_test_path(wav, base, ".wav.part");
  recovery_test_path(idx, base, ".idx.part");
  uint8_t pcm[640];
  for (size_t i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(0xa7u - i);
  recovery_test_wav(fake, wav, pcm, sizeof(pcm), 4096, 0);
  mc100_index_header_t header = {0};
  header.flags = MC100_INDEX_CLAIMED;
  memcpy(header.boot_id, boot, sizeof(boot));
  header.generation = 12;
  header.first_source_sample = 64000;
  mc100_index_record_t record = {0};
  record.type = MC100_INDEX_BLOCK;
  record.valid_bytes = sizeof(pcm);
  record.payload_crc32 = mc100_crc32(pcm, sizeof(pcm));
  record.generation = 12;
  record.first_source_sample = 64000;
  recovery_test_idx(fake, idx, &header, &record, 1,
                    4 * MC100_INDEX_RECORD_BYTES);
}

static void assert_no_mutation_since(const mc100_fake_io_t *fake,
                                     size_t start) {
  for (size_t i = start; i < mc100_fake_io_log_count(fake); ++i) {
    char kind = mc100_fake_io_log(fake, i)->kind;
    assert(kind != 'X' && kind != 'W' && kind != 'A' && kind != 'T' &&
           kind != 'S' && kind != 'N');
  }
}

static void second_run_validates_without_copying(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  create_candidate(fake, base, wav, idx);
  recovery_test_path(recovered, base, ".recovered.wav");
  uint32_t wav_hash = recovery_test_hash(fake, wav);
  uint32_t idx_hash = recovery_test_hash(fake, idx);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 1 && report.valid_pcm_bytes == 640);
  uint32_t recovered_hash = recovery_test_hash(fake, recovered);

  size_t before = mc100_fake_io_log_count(fake);
  memset(&report, 0xa5, sizeof(report));
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 && report.invalid == 0);
  assert(report.valid_pcm_bytes == 0 && report.timed_out == 0);
  assert_no_mutation_since(fake, before);
  assert(recovery_test_hash(fake, recovered) == recovered_hash);
  assert(recovery_test_hash(fake, wav) == wav_hash);
  assert(recovery_test_hash(fake, idx) == idx_hash);

  mc100_file_t file = NULL;
  assert(mc100_fake_io_ops()->open_update(fake, recovered, &file) == MC100_OK);
  uint8_t corrupt = 0;
  size_t actual = 0;
  assert(mc100_fake_io_ops()->write_at(fake, file, MC100_WAV_HEADER_BYTES + 17,
                                       &corrupt, 1, &actual) == MC100_OK);
  assert(actual == 1);
  assert(mc100_fake_io_ops()->sync(fake, file) == MC100_OK);
  assert(mc100_fake_io_ops()->close(fake, file) == MC100_OK);
  uint32_t collision_hash = recovery_test_hash(fake, recovered);
  before = mc100_fake_io_log_count(fake);
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 && report.invalid == 1);
  assert_no_mutation_since(fake, before);
  assert(recovery_test_hash(fake, recovered) == collision_hash);
  mc100_fake_io_destroy(fake);
}

static void interrupted_temporary_output_is_never_overwritten(void) {
  mc100_fake_io_t *control = mc100_fake_io_create(UINT64_C(100000000));
  assert(control != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_candidate(control, base, wav, idx);
  size_t start = mc100_fake_io_log_count(control);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), control, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  char temporary[MC100_PATH_BYTES];
  recovery_test_path(temporary, base, ".recovering_0.wav.part");
  size_t sync_relative = 0;
  for (size_t i = start; i < mc100_fake_io_log_count(control); ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(control, i);
    if (op->kind == 'S' && !strcmp(op->path, temporary)) {
      sync_relative = i - start + 1;
      break;
    }
  }
  assert(sync_relative != 0);
  mc100_fake_io_destroy(control);

  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  create_candidate(fake, base, wav, idx);
  uint32_t wav_hash = recovery_test_hash(fake, wav);
  uint32_t idx_hash = recovery_test_hash(fake, idx);
  start = mc100_fake_io_log_count(fake);
  mc100_fake_io_fault(fake, sync_relative, MC100_IO, false);
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_IO);
  assert(recovery_test_exists(fake, temporary));
  uint32_t temporary_hash = recovery_test_hash(fake, temporary);
  char recovered[MC100_PATH_BYTES];
  recovery_test_path(recovered, base, ".recovered.wav");
  assert(!recovery_test_exists(fake, recovered));

  mc100_fake_io_fault(fake, 0, MC100_OK, false);
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 1 && recovery_test_exists(fake, recovered));
  assert(recovery_test_hash(fake, temporary) == temporary_hash);
  assert(recovery_test_hash(fake, wav) == wav_hash);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  bool used_next_attempt = false;
  for (size_t i = start; i < mc100_fake_io_log_count(fake); ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(fake, i);
    if (op->kind == 'X' && strstr(op->path, ".recovering_1.wav.part"))
      used_next_attempt = true;
  }
  assert(used_next_attempt);
  mc100_fake_io_destroy(fake);
}

static void deadline_is_injected_and_nonblocking(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES];
  create_candidate(fake, base, wav, idx);
  recovery_test_path(recovered, base, ".recovered.wav");
  advancing_clock_t clock = {.now = 100, .step = 1};
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 103, advancing_now, &clock,
                       &report) == MC100_TIMEOUT);
  assert(report.recovered == 0 && report.timed_out == 1);
  assert(!recovery_test_exists(fake, recovered));
  mc100_fake_io_destroy(fake);
}

static void deadline_stops_directory_visit_at_first_expired_entry(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_candidate(fake, base, wav, idx);
  mc100_io_t io = *mc100_fake_io_ops();
  io.list = timed_list;
  uint64_t now = 0;
  list_clock = &now;
  list_visits = 0;
  advance_after_list = false;
  mc100_recovery_report_t report;
  assert(mc100_recover(&io, fake, 1, recovery_test_now, &now,
                       &report) == MC100_TIMEOUT);
  assert(report.timed_out == 1 && list_visits == 1);
  list_clock = NULL;
  mc100_fake_io_destroy(fake);
}

static void deadline_is_checked_after_empty_candidate_list(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  const uint8_t boot[16] = {0x45};
  char base[MC100_PATH_BYTES], recovered[MC100_PATH_BYTES];
  recovery_test_base(base, boot, 13, 0);
  recovery_test_path(recovered, base, ".recovered.wav");
  recovery_test_finish(fake, recovery_test_create(fake, recovered, 1));
  mc100_io_t io = *mc100_fake_io_ops();
  io.list = timed_list;
  uint64_t now = 0;
  list_clock = &now;
  list_visits = 0;
  advance_after_list = true;
  mc100_recovery_report_t report;
  assert(mc100_recover(&io, fake, 2, recovery_test_now, &now,
                       &report) == MC100_TIMEOUT);
  assert(report.timed_out == 1 && list_visits == 1);
  list_clock = NULL;
  mc100_fake_io_destroy(fake);
}

static void deadline_crossed_during_sync_prevents_recovered_publication(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char recovered[MC100_PATH_BYTES], temporary[MC100_PATH_BYTES];
  create_candidate(fake, base, wav, idx);
  recovery_test_path(recovered, base, ".recovered.wav");
  recovery_test_path(temporary, base, ".recovering_0.wav.part");
  mc100_io_t io = *mc100_fake_io_ops();
  io.sync = timed_sync;
  uint64_t now = 0;
  publication_clock = &now;
  mc100_recovery_report_t report;
  assert(mc100_recover(&io, fake, 1, recovery_test_now, &now,
                       &report) == MC100_TIMEOUT);
  assert(report.timed_out == 1 && report.recovered == 0);
  assert(recovery_test_exists(fake, temporary));
  assert(!recovery_test_exists(fake, recovered));
  publication_clock = NULL;
  mc100_fake_io_destroy(fake);
}

static void create_terminal_candidate(mc100_fake_io_t *fake, uint8_t identity,
                                      const char *wav_suffix,
                                      const char *idx_suffix, bool incident,
                                      char base[MC100_PATH_BYTES],
                                      char wav[MC100_PATH_BYTES],
                                      char idx[MC100_PATH_BYTES]) {
  uint8_t boot[16] = {0};
  boot[0] = identity;
  recovery_test_base(base, boot, identity, 0);
  recovery_test_path(wav, base, wav_suffix);
  recovery_test_path(idx, base, idx_suffix);
  uint8_t pcm[640];
  for (size_t i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(identity + i * 3u);
  recovery_test_wav(fake, wav, pcm, sizeof(pcm), sizeof(pcm), sizeof(pcm));
  mc100_index_header_t header = {0};
  header.flags = MC100_INDEX_CLAIMED;
  memcpy(header.boot_id, boot, sizeof(boot));
  header.generation = identity;
  header.first_source_sample = 96000;
  mc100_index_record_t records[2] = {0};
  records[0].type = MC100_INDEX_BLOCK;
  records[0].valid_bytes = sizeof(pcm);
  records[0].payload_crc32 = mc100_crc32(pcm, sizeof(pcm));
  records[0].generation = identity;
  records[0].first_source_sample = header.first_source_sample;
  records[1].type = incident ? MC100_INDEX_INCIDENT : MC100_INDEX_FINAL;
  records[1].journal_seq = 1;
  records[1].pcm_offset = sizeof(pcm);
  records[1].first_source_sample =
      header.first_source_sample + sizeof(pcm) / 2;
  records[1].generation = identity;
  if (incident) {
    records[1].flags = MC100_INDEX_INCOMPLETE;
    records[1].detail = MC100_INCIDENT_STORAGE_IO;
  }
  recovery_test_idx(fake, idx, &header, records, 2,
                    2 * MC100_INDEX_RECORD_BYTES);
}

static void deadline_crossed_during_close_prevents_final_publication(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char final_wav[MC100_PATH_BYTES], final_idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x64, ".wav.part", ".idx.part", false,
                            base, wav, idx);
  recovery_test_path(final_wav, base, ".wav");
  recovery_test_path(final_idx, base, ".idx");
  mc100_io_t io = *mc100_fake_io_ops();
  io.close = timed_close;
  uint64_t now = 0;
  publication_clock = &now;
  mc100_recovery_report_t report;
  assert(mc100_recover(&io, fake, 1, recovery_test_now, &now,
                       &report) == MC100_TIMEOUT);
  assert(report.timed_out == 1);
  assert(recovery_test_exists(fake, wav) && recovery_test_exists(fake, idx));
  assert(!recovery_test_exists(fake, final_wav) &&
         !recovery_test_exists(fake, final_idx));
  publication_clock = NULL;
  mc100_fake_io_destroy(fake);
}

static void deadline_between_final_renames_stops_second_publication(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  char final_wav[MC100_PATH_BYTES], final_idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x65, ".wav.part", ".idx.part", false,
                            base, wav, idx);
  recovery_test_path(final_wav, base, ".wav");
  recovery_test_path(final_idx, base, ".idx");
  mc100_io_t io = *mc100_fake_io_ops();
  io.rename_no_replace = timed_rename;
  uint64_t now = 0;
  publication_clock = &now;
  mc100_recovery_report_t report;
  assert(mc100_recover(&io, fake, 1, recovery_test_now, &now,
                       &report) == MC100_TIMEOUT);
  assert(report.timed_out == 1);
  assert(recovery_test_exists(fake, final_wav));
  assert(recovery_test_exists(fake, idx));
  assert(!recovery_test_exists(fake, wav) &&
         !recovery_test_exists(fake, final_idx));
  publication_clock = NULL;
  mc100_fake_io_destroy(fake);
}

static void finalized_naming_states_converge_without_copy(void) {
  static const struct {
    const char *wav_suffix;
    const char *idx_suffix;
  } states[] = {{".wav", ".idx.part"},
                {".wav.part", ".idx"},
                {".wav.part", ".idx.part"}};
  for (size_t state = 0; state < sizeof(states) / sizeof(states[0]); ++state) {
    mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
    assert(fake != NULL);
    char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
    create_terminal_candidate(fake, (uint8_t)(0x61 + state),
                              states[state].wav_suffix,
                              states[state].idx_suffix, false, base, wav, idx);
    uint32_t wav_hash = recovery_test_hash(fake, wav);
    uint32_t idx_hash = recovery_test_hash(fake, idx);
    uint64_t now = 0;
    mc100_recovery_report_t report;
    assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                         recovery_test_now, &now, &report) == MC100_OK);
    assert(report.recovered == 0 && report.preserved == 1 &&
           report.invalid == 0);
    char final_wav[MC100_PATH_BYTES], final_idx[MC100_PATH_BYTES];
    char recovered[MC100_PATH_BYTES];
    recovery_test_path(final_wav, base, ".wav");
    recovery_test_path(final_idx, base, ".idx");
    recovery_test_path(recovered, base, ".recovered.wav");
    assert(recovery_test_hash(fake, final_wav) == wav_hash);
    assert(recovery_test_hash(fake, final_idx) == idx_hash);
    assert(!recovery_test_exists(fake, recovered));
    assert(mc100_fake_io_count(fake) == 2);
    mc100_fake_io_destroy(fake);
  }
}

static void complete_history_uses_the_fast_path(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x71, ".wav", ".idx", false, base, wav, idx);
  size_t start = mc100_fake_io_log_count(fake);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 &&
         report.invalid == 0);
  for (size_t i = start; i < mc100_fake_io_log_count(fake); ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(fake, i);
    if (op->kind == 'R' && !strcmp(op->path, wav))
      assert(op->offset == 0 && op->length == MC100_WAV_HEADER_BYTES);
  }
  mc100_fake_io_destroy(fake);
}

static void quick_final_closes_index_after_wav_close_error(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x73, ".wav.part", ".idx.part", false,
                            base, wav, idx);
  size_t start = mc100_fake_io_log_count(fake);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  size_t wav_close = 0;
  for (size_t i = start; i < mc100_fake_io_log_count(fake); ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(fake, i);
    if (op->kind == 'C' && !strcmp(op->path, wav)) {
      wav_close = i - start + 1;
      break;
    }
  }
  assert(wav_close != 0);
  mc100_fake_io_destroy(fake);

  fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  create_terminal_candidate(fake, 0x73, ".wav.part", ".idx.part", false,
                            base, wav, idx);
  start = mc100_fake_io_log_count(fake);
  mc100_fake_io_fault(fake, wav_close, MC100_IO, false);
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_IO);
  bool index_closed = false;
  for (size_t i = start; i < mc100_fake_io_log_count(fake); ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(fake, i);
    if (op->kind == 'C' && !strcmp(op->path, idx))
      index_closed = true;
    assert(op->kind != 'N');
  }
  assert(index_closed);
  assert(recovery_test_exists(fake, wav) && recovery_test_exists(fake, idx));
  mc100_fake_io_destroy(fake);

  fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  create_terminal_candidate(fake, 0x73, ".wav.part", ".idx.part", false,
                            base, wav, idx);
  mc100_io_t io = *mc100_fake_io_ops();
  io.close = fail_both_closes;
  faulting_close_calls = 0;
  assert(mc100_recover(&io, fake, 30000, recovery_test_now, &now,
                       &report) == MC100_IO);
  assert(faulting_close_calls == 2);
  mc100_fake_io_destroy(fake);
}

static void completed_incident_remains_partial(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x72, ".partial.wav", ".idx", true, base,
                            wav, idx);
  uint32_t wav_hash = recovery_test_hash(fake, wav);
  uint32_t idx_hash = recovery_test_hash(fake, idx);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 1 &&
         report.invalid == 0);
  char recovered[MC100_PATH_BYTES];
  recovery_test_path(recovered, base, ".recovered.wav");
  assert(!recovery_test_exists(fake, recovered));
  assert(recovery_test_hash(fake, wav) == wav_hash);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  mc100_fake_io_destroy(fake);
}

static void corrupt_incident_header_recovers_trusted_pcm(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  char base[MC100_PATH_BYTES], wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  create_terminal_candidate(fake, 0x74, ".partial.wav", ".idx", true, base,
                            wav, idx);
  mc100_file_t file = NULL;
  assert(mc100_fake_io_ops()->open_update(fake, wav, &file) == MC100_OK);
  uint8_t bad = 0;
  size_t actual = 0;
  assert(mc100_fake_io_ops()->write_at(fake, file, 0, &bad, 1, &actual) ==
         MC100_OK);
  assert(actual == 1 && mc100_fake_io_ops()->sync(fake, file) == MC100_OK);
  assert(mc100_fake_io_ops()->close(fake, file) == MC100_OK);
  uint32_t source_hash = recovery_test_hash(fake, wav);
  uint32_t idx_hash = recovery_test_hash(fake, idx);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 1 && report.valid_pcm_bytes == 640);
  char recovered[MC100_PATH_BYTES];
  recovery_test_path(recovered, base, ".recovered.wav");
  size_t size = 0;
  const uint8_t *output = mc100_fake_io_bytes(fake, recovered, &size);
  uint8_t header[MC100_WAV_HEADER_BYTES];
  assert(mc100_wav_header(header, 640) == MC100_OK);
  assert(output != NULL && size == MC100_WAV_HEADER_BYTES + 640);
  assert(memcmp(output, header, sizeof(header)) == 0);
  assert(recovery_test_hash(fake, wav) == source_hash);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  mc100_fake_io_destroy(fake);
}

static void reserved_slots_stay_reusable_without_growth(void) {
  const uint8_t boot[16] = {0x73};
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  mc100_writer_t *writer =
      mc100_writer_create(mc100_fake_io_ops(), fake, boot);
  assert(writer != NULL && mc100_writer_prepare(writer) == MC100_OK);
  mc100_writer_destroy(writer);
  assert(mc100_fake_io_count(fake) == 4);
  const size_t before_count = mc100_fake_io_count(fake);
  uint64_t total = 0, before_free = 0;
  assert(mc100_fake_io_ops()->space(fake, &total, &before_free) == MC100_OK);
  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 2 &&
         report.invalid == 0);
  writer = mc100_writer_create(mc100_fake_io_ops(), fake, boot);
  assert(writer != NULL && mc100_writer_prepare(writer) == MC100_OK);
  mc100_writer_destroy(writer);
  uint64_t after_free = 0;
  assert(mc100_fake_io_ops()->space(fake, &total, &after_free) == MC100_OK);
  assert(mc100_fake_io_count(fake) == before_count);
  assert(after_free == before_free);
  mc100_fake_io_destroy(fake);
}

static void claimed_reserve_is_not_reused_as_empty(void) {
  const uint8_t boot[16] = {0x75};
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  mc100_writer_t *writer =
      mc100_writer_create(mc100_fake_io_ops(), fake, boot);
  assert(writer != NULL && mc100_writer_prepare(writer) == MC100_OK);
  mc100_writer_destroy(writer);
  const char *idx =
      "75000000000000000000000000000000_reserve_0.idx.part";
  const char *wav =
      "75000000000000000000000000000000_reserve_0.wav.part";
  mc100_index_header_t claimed = {0};
  claimed.flags = MC100_INDEX_CLAIMED;
  memcpy(claimed.boot_id, boot, sizeof(boot));
  claimed.generation = 9;
  claimed.first_source_sample = 32000;
  uint8_t encoded[MC100_INDEX_HEADER_BYTES];
  assert(mc100_index_header_encode(encoded, &claimed) == MC100_OK);
  mc100_file_t file = NULL;
  assert(mc100_fake_io_ops()->open_update(fake, idx, &file) == MC100_OK);
  recovery_test_write(fake, file, 0, encoded, sizeof(encoded));
  recovery_test_finish(fake, file);
  uint32_t idx_hash = recovery_test_hash(fake, idx);
  uint32_t wav_hash = recovery_test_hash(fake, wav);

  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.invalid == 1);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  assert(recovery_test_hash(fake, wav) == wav_hash);

  writer = mc100_writer_create(mc100_fake_io_ops(), fake, boot);
  assert(writer != NULL && mc100_writer_prepare(writer) == MC100_OK);
  assert(mc100_writer_begin(writer, 10, 100) == MC100_OK);
  mc100_writer_destroy(writer);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  assert(recovery_test_hash(fake, wav) == wav_hash);
  mc100_fake_io_destroy(fake);
}

static void dirty_reserved_index_tail_is_invalid(void) {
  const uint8_t boot[16] = {0x76};
  mc100_fake_io_t *fake = mc100_fake_io_create(UINT64_C(100000000));
  assert(fake != NULL);
  mc100_writer_t *writer =
      mc100_writer_create(mc100_fake_io_ops(), fake, boot);
  assert(writer != NULL && mc100_writer_prepare(writer) == MC100_OK);
  mc100_writer_destroy(writer);
  const char *idx =
      "76000000000000000000000000000000_reserve_0.idx.part";
  mc100_file_t file = NULL;
  assert(mc100_fake_io_ops()->open_update(fake, idx, &file) == MC100_OK);
  const uint8_t dirty = 0x5a;
  recovery_test_write(fake, file, MC100_INDEX_HEADER_BYTES + 123, &dirty, 1);
  recovery_test_finish(fake, file);
  uint32_t idx_hash = recovery_test_hash(fake, idx);

  uint64_t now = 0;
  mc100_recovery_report_t report;
  assert(mc100_recover(mc100_fake_io_ops(), fake, 30000,
                       recovery_test_now, &now, &report) == MC100_OK);
  assert(report.recovered == 0 && report.preserved == 2 &&
         report.invalid == 1);
  assert(recovery_test_hash(fake, idx) == idx_hash);
  mc100_fake_io_destroy(fake);
}

int main(void) {
  second_run_validates_without_copying();
  interrupted_temporary_output_is_never_overwritten();
  deadline_is_injected_and_nonblocking();
  deadline_stops_directory_visit_at_first_expired_entry();
  deadline_is_checked_after_empty_candidate_list();
  deadline_crossed_during_sync_prevents_recovered_publication();
  deadline_crossed_during_close_prevents_final_publication();
  deadline_between_final_renames_stops_second_publication();
  finalized_naming_states_converge_without_copy();
  complete_history_uses_the_fast_path();
  quick_final_closes_index_after_wav_close_error();
  completed_incident_remains_partial();
  corrupt_incident_header_recovers_trusted_pcm();
  reserved_slots_stay_reusable_without_growth();
  claimed_reserve_is_not_reused_as_empty();
  dirty_reserved_index_tail_is_invalid();
  puts("recover_idempotent: validated output, immutable collision/temp and "
       "injected deadline PASS");
  return 0;
}
