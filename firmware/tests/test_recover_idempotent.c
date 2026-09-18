#include "recovery_test_support.h"

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

int main(void) {
  second_run_validates_without_copying();
  interrupted_temporary_output_is_never_overwritten();
  deadline_is_injected_and_nonblocking();
  puts("recover_idempotent: validated output, immutable collision/temp and "
       "injected deadline PASS");
  return 0;
}
