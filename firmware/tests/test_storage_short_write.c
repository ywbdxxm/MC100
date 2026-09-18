#include "mc100_fake_io.h"
#include "mc100_writer.h"
#include <assert.h>
#include <string.h>
static mc100_writer_t *opened(mc100_fake_io_t *f) {
  const uint8_t id[16] = {2};
  mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, id);
  assert(w);
  assert(mc100_writer_prepare(w) == MC100_OK);
  assert(mc100_writer_begin(w, 1, 0) == MC100_OK);
  mc100_packet_t p = {0};
  p.generation = 1;
  for (uint64_t i = 0; i < 8; ++i) {
    p.frame.seq = i;
    assert(mc100_writer_append(w, &p) == MC100_OK);
  }
  return w;
}
static void injected_checkpoint(size_t operation, bool short_write) {
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = opened(f);
  mc100_fake_io_fault(f, operation, short_write ? MC100_OK : MC100_IO,
                      short_write);
  assert(mc100_writer_checkpoint(w, 1000) == MC100_IO);
  mc100_writer_status_t s;
  assert(mc100_writer_status(w, &s) == MC100_OK);
  assert(s.committed_bytes == 0 && s.accepted_bytes == 5120);
  mc100_packet_t p = {0};
  p.generation = 1;
  p.frame.seq = 8;
  size_t before = mc100_fake_io_log_count(f);
  assert(mc100_writer_append(w, &p) == MC100_IO);
  assert(mc100_writer_close_through(w, 1, 7, 0) == MC100_IO);
  mc100_writer_destroy(w);
  assert(before == mc100_fake_io_log_count(f));
  mc100_fake_io_destroy(f);
}
static void check_incident(uint32_t reason, bool space_full) {
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = opened(f);
  if (space_full) {
    mc100_fake_io_free_bytes(f, 99999999);
    mc100_packet_t p = {0};
    p.generation = 1;
    p.frame.seq = 8;
    assert(mc100_writer_append(w, &p) == MC100_FULL);
  } else
    assert(mc100_writer_close_through(w, 1, 7, reason) == MC100_OK);
  bool audio = false, index = false;
  for (size_t i = 0; i < mc100_fake_io_count(f); ++i) {
    const char *p = mc100_fake_io_path(f, i);
    size_t n;
    const uint8_t *b = mc100_fake_io_bytes(f, p, &n);
    if (strstr(p, ".partial.wav")) {
      assert(n == 5632);
      audio = true;
    }
    size_t l = strlen(p);
    if (l > 4 && !strcmp(p + l - 4, ".idx")) {
      mc100_index_record_t r;
      assert(mc100_index_decode(b + n - 64, &r) == MC100_OK);
      assert(r.type == MC100_INDEX_INCIDENT && r.flags == 1 &&
             r.detail == reason && r.pcm_offset == 5120);
      index = true;
    }
  }
  assert(audio && index);
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
}
static void ordering_and_validation(void) {
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = opened(f);
  mc100_packet_t p = {0};
  p.generation = 2;
  p.frame.seq = 8;
  size_t before = mc100_fake_io_log_count(f);
  assert(mc100_writer_append(w, &p) == MC100_INVALID);
  p.generation = 1;
  p.frame.seq = 7;
  assert(mc100_writer_append(w, &p) == MC100_INVALID);
  p.frame.seq = 9;
  assert(mc100_writer_append(w, &p) == MC100_INVALID);
  assert(before == mc100_fake_io_log_count(f));
  assert(mc100_writer_checkpoint(w, 999) == MC100_OK);
  assert(before == mc100_fake_io_log_count(f));
  assert(mc100_writer_checkpoint(w, 1000) == MC100_OK);
  const char expected[] = {'W', 'S', 'W', 'W', 'S'};
  assert(mc100_fake_io_log_count(f) == before + 5);
  for (size_t i = 0; i < 5; ++i) {
    const mc100_fake_op_t *op = mc100_fake_io_log(f, before + i);
    assert(op->kind == expected[i]);
    assert(strstr(op->path, i < 2 ? ".wav.part" : ".idx.part"));
  }
  const mc100_fake_op_t *op = mc100_fake_io_log(f, before);
  assert(op->offset == 512 && op->length == 4096);
  mc100_writer_status_t s;
  assert(mc100_writer_status(w, &s) == MC100_OK);
  assert(s.committed_bytes == 4096 && s.accepted_bytes == 5120);
  mc100_writer_abandon(w, MC100_INCIDENT_LOW_BAT_INTERRUPTED);
  before = mc100_fake_io_log_count(f);
  assert(mc100_writer_checkpoint(w, 2000) == MC100_NOT_READY);
  assert(mc100_writer_close_through(w, 1, 7, 9) == MC100_NOT_READY);
  assert(before == mc100_fake_io_log_count(f));
  assert(mc100_writer_release_handles(w) == MC100_OK);
  for (size_t i = before; i < mc100_fake_io_log_count(f); ++i)
    assert(mc100_fake_io_log(f, i)->kind == 'C');
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
}
static void short_allocation(void) {
  const uint8_t id[16] = {2};
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, id);
  mc100_fake_io_fault(f, 6, MC100_OK, true);
  assert(mc100_writer_prepare(w) == MC100_FULL);
  mc100_writer_status_t s;
  assert(mc100_writer_status(w, &s) == MC100_OK);
  assert(!s.prepared_slots);
  assert(mc100_writer_release_handles(w) == MC100_OK);
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
}
int main(void) {
  for (size_t fail = 1; fail <= 5; ++fail)
    injected_checkpoint(fail, false);
  injected_checkpoint(1, true);
  injected_checkpoint(3, true);
  injected_checkpoint(4, true);
  check_incident(MC100_INCIDENT_QUEUE_OVERFLOW, false);
  check_incident(MC100_INCIDENT_STORAGE_FULL, true);
  ordering_and_validation();
  short_allocation();
  return 0;
}
