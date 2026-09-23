#include "mc100_fake_io.h"
#include "mc100_writer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define RECORD_BASE "08000000000000000000000000000000_3_0"
static const char wav_part[] = RECORD_BASE ".wav.part";
static const char idx_part[] = RECORD_BASE ".idx.part";
static const char wav_final[] = RECORD_BASE ".wav";
static const char idx_final[] = RECORD_BASE ".idx";

/* A two-block tail catches skipped/unchecked writes, sync, truncation, close,
 * and either rename. Expected ordering comes from the transaction contract. */
static const char close_operations[] = {'W', 'S', 'W', 'W', 'W', 'S', 'T', 'W',
                                        'S', 'T', 'S', 'C', 'C', 'N', 'N'};

static mc100_writer_t *recording(mc100_fake_io_t *fake) {
  const uint8_t identity[16] = {8};
  mc100_writer_t *writer =
      mc100_writer_create(mc100_fake_io_ops(), fake, identity);
  assert(writer);
  assert(mc100_writer_prepare(writer) == MC100_OK);
  assert(mc100_writer_begin(writer, 3, 100) == MC100_OK);
  mc100_packet_t packet = {0};
  packet.generation = 3;
  for (uint64_t seq = 100; seq < 108; ++seq) {
    packet.frame.seq = seq;
    for (size_t i = 0; i < MC100_FRAME_SAMPLES; ++i)
      packet.frame.pcm[i] = (int16_t)(seq + i);
    assert(mc100_writer_append(writer, &packet) == MC100_OK);
  }
  return writer;
}

static bool exists(const mc100_fake_io_t *fake, const char *path) {
  for (size_t i = 0; i < mc100_fake_io_count(fake); ++i)
    if (!strcmp(mc100_fake_io_path(fake, i), path))
      return true;
  return false;
}

static void assert_latched(mc100_writer_t *writer, mc100_fake_io_t *fake,
                           mc100_result_t expected) {
  const size_t before = mc100_fake_io_log_count(fake);
  mc100_packet_t next = {0};
  next.generation = 3;
  next.frame.seq = 108;
  assert(mc100_writer_append(writer, &next) == expected);
  assert(mc100_writer_checkpoint(writer, 1000) == expected);
  assert(mc100_writer_close_through(writer, 3, 107, 0) == expected);
  assert(mc100_writer_prepare(writer) == expected);
  assert(mc100_writer_begin(writer, 4, 108) == expected);
  assert(mc100_fake_io_log_count(fake) == before);
}

static void release_and_destroy(mc100_writer_t *writer, mc100_fake_io_t *fake,
                                size_t retained_handles) {
  size_t before = mc100_fake_io_log_count(fake);
  assert(mc100_writer_release_handles(writer) == MC100_OK);
  assert(mc100_fake_io_log_count(fake) == before + retained_handles);
  for (size_t i = before; i < mc100_fake_io_log_count(fake); ++i)
    assert(mc100_fake_io_log(fake, i)->kind == 'C');
  before = mc100_fake_io_log_count(fake);
  assert(mc100_writer_release_handles(writer) == MC100_OK);
  mc100_writer_destroy(writer);
  assert(mc100_fake_io_log_count(fake) == before);
  mc100_fake_io_destroy(fake);
}

static void finalize_failure(size_t position, bool short_write) {
  mc100_fake_io_t *fake = mc100_fake_io_create(1000000000);
  assert(fake);
  mc100_writer_t *writer = recording(fake);
  const size_t start = mc100_fake_io_log_count(fake);
  mc100_fake_io_fault(fake, position, short_write ? MC100_OK : MC100_IO,
                      short_write);
  assert(mc100_writer_close_through(writer, 3, 107, 0) == MC100_IO);
  assert(mc100_fake_io_log_count(fake) == start + position);
  for (size_t i = 0; i < position; ++i)
    assert(mc100_fake_io_log(fake, start + i)->kind == close_operations[i]);

  mc100_writer_status_t status;
  assert(mc100_writer_status(writer, &status) == MC100_OK);
  assert(status.accepted_bytes == 5120 && status.last_seq == 107);
  assert(status.committed_bytes == (position <= 6 ? 0u : 5120u));
  assert(status.latched_reason == MC100_INCIDENT_STORAGE_IO);
  assert(status.active);
  /* Only failure of the second rename permits a finalized WAV pathname;
   * the unrenamed index remains available as evidence in every failure. */
  assert(exists(fake, wav_part) == (position < 15));
  assert(exists(fake, wav_final) == (position == 15));
  assert(exists(fake, idx_part) && !exists(fake, idx_final));
  assert(mc100_fake_io_count(fake) == 4);
  mc100_writer_publication_t publication = {0};
  assert(mc100_writer_publication_pop(writer, &publication) ==
         MC100_NOT_READY);
  assert_latched(writer, fake, MC100_IO);
  release_and_destroy(writer, fake,
                      position <= 12   ? 2
                      : position == 13 ? 1
                                       : 0);
}

static void final_collision(bool index_collision) {
  mc100_fake_io_t *fake = mc100_fake_io_create(1000000000);
  assert(fake);
  mc100_writer_t *writer = recording(fake);
  /* Inject an external file appearing after begin's namespace admission. */
  const char *destination = index_collision ? idx_final : wav_final;
  const uint8_t preserved[] = {0x71, 0x00, 0x93, 0x42, 0xff};
  const mc100_io_t *io = mc100_fake_io_ops();
  mc100_file_t file = NULL;
  assert(io->open_exclusive(fake, destination, &file) == MC100_OK);
  size_t actual = 0;
  assert(io->write_at(fake, file, 0, preserved, sizeof(preserved), &actual) ==
         MC100_OK);
  assert(actual == sizeof(preserved));
  assert(io->sync(fake, file) == MC100_OK);
  assert(io->close(fake, file) == MC100_OK);

  assert(mc100_writer_close_through(writer, 3, 107, 0) == MC100_NOT_READY);
  size_t size = 0;
  const uint8_t *bytes = mc100_fake_io_bytes(fake, destination, &size);
  assert(bytes && size == sizeof(preserved));
  assert(!memcmp(bytes, preserved, size));
  assert(exists(fake, idx_part));
  assert(exists(fake, wav_part) == !index_collision);
  assert_latched(writer, fake, MC100_NOT_READY);
  release_and_destroy(writer, fake, 0);
}

static void failed_release_remains_retryable(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(1000000000);
  assert(fake);
  mc100_writer_t *writer = recording(fake);
  mc100_fake_io_fault(fake, 1, MC100_IO, false);
  assert(mc100_writer_close_through(writer, 3, 107, 0) == MC100_IO);
  const size_t before = mc100_fake_io_log_count(fake);
  mc100_fake_io_fault(fake, 1, MC100_IO, false);
  assert(mc100_writer_release_handles(writer) == MC100_IO);
  assert(mc100_fake_io_log_count(fake) == before + 2);
  for (size_t i = before; i < before + 2; ++i)
    assert(mc100_fake_io_log(fake, i)->kind == 'C');
  release_and_destroy(writer, fake, 1);
}

static void normal_close_control(void) {
  mc100_fake_io_t *fake = mc100_fake_io_create(1000000000);
  assert(fake);
  mc100_writer_t *writer = recording(fake);
  const size_t start = mc100_fake_io_log_count(fake);
  assert(mc100_writer_close_through(writer, 3, 107, 0) == MC100_OK);
  assert(mc100_fake_io_log_count(fake) == start + sizeof(close_operations));
  for (size_t i = 0; i < sizeof(close_operations); ++i)
    assert(mc100_fake_io_log(fake, start + i)->kind == close_operations[i]);
  size_t size = 0;
  const uint8_t *bytes = mc100_fake_io_bytes(fake, wav_final, &size);
  assert(bytes && size == 5632);
  for (size_t sample = 0; sample < 2560; ++sample) {
    uint16_t expected = (uint16_t)(100 + sample / 320 + sample % 320);
    assert(bytes[512 + sample * 2] == (uint8_t)expected);
    assert(bytes[513 + sample * 2] == (uint8_t)(expected >> 8));
  }
  bytes = mc100_fake_io_bytes(fake, idx_final, &size);
  assert(bytes && size == 704);
  mc100_index_record_t final;
  assert(mc100_index_decode(bytes + 640, &final) == MC100_OK);
  assert(final.type == MC100_INDEX_FINAL && final.pcm_offset == 5120);
  assert(final.flags == 0 && final.detail == 0);
  assert(!exists(fake, wav_part) && !exists(fake, idx_part));
  mc100_writer_publication_t publication = {0};
  assert(mc100_writer_publication_pop(writer, &publication) == MC100_OK);
  assert(!strcmp(publication.name, wav_final));
  assert(publication.generation == 3 && publication.segment_index == 0);
  assert(mc100_writer_publication_pop(writer, &publication) ==
         MC100_NOT_READY);
  release_and_destroy(writer, fake, 0);
}

int main(void) {
  normal_close_control();
  for (size_t position = 1; position <= sizeof(close_operations); ++position) {
    finalize_failure(position, false);
    if (close_operations[position - 1] == 'W')
      finalize_failure(position, true);
  }
  final_collision(false);
  final_collision(true);
  failed_release_remains_retryable();
  puts("finalize_faults: 15 hard failures, 5 short writes, 2 collisions, "
       "release retry and normal bytes PASS");
  return 0;
}
