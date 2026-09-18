#include "mc100_writer.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WAV_SIZE = 9600512, IDX_SIZE = 262656 };
typedef struct {
  char wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
} slot_t;
struct mc100_writer {
  mc100_io_t io;
  void *ctx;
  uint8_t boot[16];
  char identity[33];
  slot_t slots[2], current;
  uint32_t count, counter, segment;
  mc100_file_t wav, idx, pending_wav, pending_idx;
  mc100_index_validation_t validation;
  uint8_t staging[MC100_STAGING_BYTES];
  size_t buffered;
  mc100_generation_t generation;
  uint64_t first_seq, last_seq, accepted, checkpoint_ms;
  uint32_t reason;
  mc100_result_t failed;
  bool active, has_frames, abandoned;
};

bool mc100_path_valid(const char *p) {
  if (!p)
    return false;
  size_t n = 0;
  while (n < MC100_PATH_BYTES && p[n]) {
    char c = p[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '.'))
      return false;
    ++n;
  }
  if (n < 38 || n >= MC100_PATH_BYTES || p[32] != '_' || strstr(p, ".."))
    return false;
  for (size_t i = 0; i < 32; ++i)
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
      return false;
  const char *q = p + 33;
  if (!strncmp(q, "reserve_", 8))
    q += 8;
  else {
    if (*q < '0' || *q > '9')
      return false;
    while (*q >= '0' && *q <= '9')
      ++q;
    if (*q++ != '_')
      return false;
  }
  if (*q < '0' || *q > '9')
    return false;
  while (*q >= '0' && *q <= '9')
    ++q;
  return !strcmp(q, ".wav.part") || !strcmp(q, ".idx.part") ||
         !strcmp(q, ".wav") || !strcmp(q, ".idx") ||
         !strcmp(q, ".partial.wav") || !strcmp(q, ".recovered.wav");
}
static uint64_t floor_space(uint64_t total) {
  return total / 10 + (total % 10 != 0);
}
bool mc100_space_can_prepare(uint64_t total, uint64_t free_bytes,
                             uint64_t reserve) {
  uint64_t floor = floor_space(total);
  return total != 0 && free_bytes <= total && reserve <= free_bytes &&
         free_bytes - reserve >= floor &&
         free_bytes - reserve - floor >= UINT64_C(1048576);
}
static mc100_result_t fail(mc100_writer_t *w, mc100_result_t r) {
  if (r != MC100_OK) {
    w->failed = r;
    if (!w->reason)
      w->reason = r == MC100_FULL ? MC100_INCIDENT_STORAGE_FULL
                                  : MC100_INCIDENT_STORAGE_IO;
  }
  return r;
}
static mc100_result_t write_exact(mc100_writer_t *w, mc100_file_t h,
                                  uint64_t off, const void *p, size_t n) {
  size_t actual = 0;
  mc100_result_t r = w->io.write_at(w->ctx, h, off, p, n, &actual);
  return r ? r : actual == n ? MC100_OK : MC100_IO;
}
static mc100_result_t read_exact(mc100_writer_t *w, mc100_file_t h,
                                 uint64_t off, void *p, size_t n) {
  size_t actual = 0;
  mc100_result_t r = w->io.read_at(w->ctx, h, off, p, n, &actual);
  return r ? r : actual == n ? MC100_OK : MC100_CORRUPT;
}
static void boot_text(char out[33], const uint8_t boot[16]) {
  static const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < 16; ++i) {
    out[i * 2] = hex[boot[i] >> 4];
    out[i * 2 + 1] = hex[boot[i] & 15];
  }
  out[32] = 0;
}
static bool paths(slot_t *slot, const char *base) {
  int a = snprintf(slot->wav, sizeof(slot->wav), "%s.wav.part", base);
  int b = snprintf(slot->idx, sizeof(slot->idx), "%s.idx.part", base);
  return a > 0 && a < MC100_PATH_BYTES && b > 0 && b < MC100_PATH_BYTES;
}
static mc100_result_t absent(mc100_writer_t *w, const char *p) {
  uint64_t n;
  mc100_result_t r = w->io.stat(w->ctx, p, &n);
  return r == MC100_NOT_READY ? MC100_OK : r == MC100_OK ? MC100_NOT_READY : r;
}
static mc100_result_t namespace_free(mc100_writer_t *w, const char *base) {
  static const char *suffix[] = {".wav.part", ".idx.part",    ".wav",
                                 ".idx",      ".partial.wav", ".recovered.wav"};
  for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); ++i) {
    char p[MC100_PATH_BYTES];
    int n = snprintf(p, sizeof(p), "%s%s", base, suffix[i]);
    if (n < 0 || n >= MC100_PATH_BYTES)
      return MC100_INVALID;
    mc100_result_t r = absent(w, p);
    if (r)
      return r;
  }
  return MC100_OK;
}
mc100_writer_t *mc100_writer_create(const mc100_io_t *io, void *ctx,
                                    const uint8_t boot[16]) {
  if (!io || !boot || !io->open_exclusive || !io->open_read ||
      !io->open_update || !io->read_at || !io->write_at || !io->allocate ||
      !io->sync || !io->truncate || !io->close || !io->rename_no_replace ||
      !io->stat || !io->space || !io->list)
    return NULL;
  mc100_writer_t *w = calloc(1, sizeof(*w));
  if (w) {
    w->io = *io;
    w->ctx = ctx;
    memcpy(w->boot, boot, 16);
    boot_text(w->identity, boot);
  }
  return w;
}
void mc100_writer_destroy(mc100_writer_t *w) { free(w); }
void mc100_writer_abandon(mc100_writer_t *w, uint32_t reason) {
  if (w) {
    w->abandoned = true;
    w->reason = reason;
    w->failed = MC100_NOT_READY;
  }
}
mc100_result_t mc100_writer_release_handles(mc100_writer_t *w) {
  if (!w)
    return MC100_INVALID;
  mc100_file_t *handles[] = {&w->wav, &w->idx, &w->pending_wav,
                             &w->pending_idx};
  w->abandoned = true;
  if (!w->failed)
    w->failed = MC100_NOT_READY;
  mc100_result_t result = MC100_OK;
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
    if (*handles[i]) {
      mc100_result_t r = w->io.close(w->ctx, *handles[i]);
      if (!r)
        *handles[i] = NULL;
      else if (!result)
        result = r;
    }
  }
  return result;
}
mc100_result_t mc100_writer_status(const mc100_writer_t *w,
                                   mc100_writer_status_t *s) {
  if (!w || !s)
    return MC100_INVALID;
  s->prepared_slots = w->count;
  s->segment_index = w->segment;
  s->latched_reason = w->reason;
  s->accepted_bytes = w->accepted;
  s->committed_bytes = w->validation.pcm_bytes;
  s->last_seq = w->last_seq;
  s->generation = w->generation;
  s->active = w->active;
  s->has_frames = w->has_frames;
  s->abandoned = w->abandoned;
  return MC100_OK;
}
static mc100_result_t reserve_visit(void *ctx, const char *p) {
  mc100_writer_t *w = ctx;
  if (!mc100_path_valid(p) || !strstr(p + 33, "reserve_") ||
      !strstr(p, ".idx.part"))
    return MC100_OK;
  if (w->count == 2)
    return MC100_OK;
  for (uint32_t i = 0; i < w->count; ++i)
    if (!strcmp(w->slots[i].idx, p))
      return MC100_OK;
  slot_t slot;
  char base[MC100_PATH_BYTES];
  size_t len = strlen(p) - 9;
  memcpy(base, p, len);
  base[len] = 0;
  if (!paths(&slot, base))
    return MC100_CORRUPT;
  uint64_t size;
  mc100_result_t r = w->io.stat(w->ctx, slot.idx, &size);
  if (r)
    return r;
  if (size != IDX_SIZE)
    return MC100_CORRUPT;
  mc100_file_t h = NULL;
  r = w->io.open_read(w->ctx, slot.idx, &h);
  if (r)
    return r;
  w->pending_idx = h;
  uint8_t block[4096];
  mc100_index_header_t header = {0};
  r = read_exact(w, h, 0, block, 512);
  if (!r)
    r = mc100_index_header_decode(block, &header);
  if (!r && header.flags == MC100_INDEX_CLAIMED) {
    r = w->io.close(w->ctx, h);
    if (!r)
      w->pending_idx = NULL;
    return r;
  }
  char identity[33];
  if (!r) {
    boot_text(identity, header.boot_id);
    if (strncmp(identity, p, 32))
      r = MC100_CORRUPT;
  }
  for (uint64_t offset = 512; !r && offset < IDX_SIZE;
       offset += sizeof(block)) {
    r = read_exact(w, h, offset, block, sizeof(block));
    for (size_t j = 0; !r && j < sizeof(block); ++j)
      if (block[j])
        r = MC100_CORRUPT;
  }
  mc100_result_t closed = w->io.close(w->ctx, h);
  if (!closed)
    w->pending_idx = NULL;
  if (r)
    return r;
  if (closed)
    return closed;
  r = w->io.stat(w->ctx, slot.wav, &size);
  if (r)
    return r;
  if (size != WAV_SIZE)
    return MC100_CORRUPT;
  r = w->io.open_read(w->ctx, slot.wav, &h);
  if (r)
    return r;
  w->pending_wav = h;
  r = read_exact(w, h, 0, block, 512);
  uint8_t empty[512];
  (void)mc100_wav_header(empty, 0);
  if (!r && memcmp(empty, block, 512))
    r = MC100_CORRUPT;
  closed = w->io.close(w->ctx, h);
  if (!closed)
    w->pending_wav = NULL;
  if (r)
    return r;
  if (closed)
    return closed;
  w->slots[w->count++] = slot;
  return MC100_OK;
}
static mc100_result_t make_slot(mc100_writer_t *w) {
  uint64_t total, available;
  mc100_result_t r = w->io.space(w->ctx, &total, &available);
  if (r)
    return r;
  if (!mc100_space_can_prepare(total, available, MC100_SLOT_BYTES))
    return MC100_FULL;
  slot_t slot;
  char base[MC100_PATH_BYTES];
  mc100_file_t wav = NULL, idx = NULL;
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    if (w->counter == UINT32_MAX)
      return MC100_NOT_READY;
    int n = snprintf(base, sizeof(base), "%s_reserve_%" PRIu32, w->identity,
                     w->counter++);
    if (n < 0 || n >= MC100_PATH_BYTES || !paths(&slot, base))
      return MC100_INVALID;
    r = absent(w, slot.idx);
    if (r == MC100_NOT_READY)
      continue;
    if (r)
      return r;
    r = w->io.open_exclusive(w->ctx, slot.wav, &w->pending_wav);
    if (r == MC100_NOT_READY)
      continue;
    if (r)
      return r;
    wav = w->pending_wav;
    break;
  }
  if (!wav)
    return MC100_NOT_READY;
  r = w->io.open_exclusive(w->ctx, slot.idx, &w->pending_idx);
  if (r)
    return r;
  idx = w->pending_idx;
  uint64_t actual = 0;
  r = w->io.allocate(w->ctx, wav, WAV_SIZE, &actual);
  if (r)
    return r;
  if (actual != WAV_SIZE)
    return MC100_FULL;
  r = w->io.allocate(w->ctx, idx, IDX_SIZE, &actual);
  if (r)
    return r;
  if (actual != IDX_SIZE)
    return MC100_FULL;
  uint8_t block[4096] = {0};
  (void)mc100_wav_header(block, 0);
  r = write_exact(w, wav, 0, block, 512);
  if (r)
    return r;
  r = w->io.sync(w->ctx, wav);
  if (r)
    return r;
  memset(block, 0, sizeof(block));
  for (uint64_t offset = 512; offset < IDX_SIZE; offset += sizeof(block)) {
    r = write_exact(w, idx, offset, block, sizeof(block));
    if (r)
      return r;
  }
  r = w->io.sync(w->ctx, idx);
  if (r)
    return r;
  mc100_index_header_t h = {0};
  h.flags = MC100_INDEX_RESERVED;
  memcpy(h.boot_id, w->boot, 16);
  r = mc100_index_header_encode(block, &h);
  if (r)
    return r;
  r = write_exact(w, idx, 0, block, 512);
  if (r)
    return r;
  r = w->io.sync(w->ctx, idx);
  if (r)
    return r;
  r = w->io.close(w->ctx, wav);
  if (r)
    return r;
  w->pending_wav = NULL;
  r = w->io.close(w->ctx, idx);
  if (r)
    return r;
  w->pending_idx = NULL;
  w->slots[w->count++] = slot;
  return MC100_OK;
}
mc100_result_t mc100_writer_prepare(mc100_writer_t *w) {
  if (!w)
    return MC100_INVALID;
  if (w->failed)
    return w->failed;
  if (w->active)
    return MC100_INVALID;
  mc100_result_t r = w->io.list(w->ctx, reserve_visit, w);
  if (r)
    return fail(w, r);
  while (w->count < 2) {
    r = make_slot(w);
    if (r)
      return fail(w, r);
  }
  return MC100_OK;
}
static mc100_result_t claim(mc100_writer_t *w, uint64_t seq) {
  if (!w->count)
    return MC100_NOT_READY;
  char base[MC100_PATH_BYTES];
  int n = snprintf(base, sizeof(base), "%s_%" PRIu64 "_%" PRIu32, w->identity,
                   w->generation, w->segment);
  if (n < 0 || n >= MC100_PATH_BYTES)
    return MC100_INVALID;
  mc100_result_t r = namespace_free(w, base);
  if (r)
    return r;
  slot_t next;
  if (!paths(&next, base))
    return MC100_INVALID;
  slot_t old = w->slots[0];
  mc100_file_t wav = NULL, idx = NULL;
  r = w->io.open_update(w->ctx, old.wav, &w->wav);
  if (r)
    return r;
  wav = w->wav;
  r = w->io.open_update(w->ctx, old.idx, &w->idx);
  if (r)
    return r;
  idx = w->idx;
  mc100_index_header_t header = {0};
  header.flags = MC100_INDEX_CLAIMED;
  header.generation = w->generation;
  header.segment_index = w->segment;
  header.first_source_sample = seq * 320;
  memcpy(header.boot_id, w->boot, 16);
  uint8_t encoded[512];
  r = mc100_index_header_encode(encoded, &header);
  if (r)
    return r;
  r = write_exact(w, idx, 0, encoded, sizeof(encoded));
  if (r)
    return r;
  r = w->io.sync(w->ctx, idx);
  if (r)
    return r;
  r = w->io.close(w->ctx, wav);
  if (r)
    return r;
  w->wav = NULL;
  r = w->io.close(w->ctx, idx);
  if (r)
    return r;
  w->idx = NULL;
  r = w->io.rename_no_replace(w->ctx, old.wav, next.wav);
  if (r)
    return r;
  r = w->io.rename_no_replace(w->ctx, old.idx, next.idx);
  if (r)
    return r;
  r = w->io.open_update(w->ctx, next.wav, &w->wav);
  if (r)
    return r;
  r = w->io.open_update(w->ctx, next.idx, &w->idx);
  if (r)
    return r;
  r = mc100_index_validation_init(&w->validation, &header);
  if (r)
    return r;
  w->current = next;
  --w->count;
  if (w->count)
    w->slots[0] = w->slots[1];
  w->first_seq = seq;
  w->accepted = 0;
  w->buffered = 0;
  w->active = true;
  return MC100_OK;
}
mc100_result_t mc100_writer_begin(mc100_writer_t *w, mc100_generation_t g,
                                  uint64_t seq) {
  if (!w || !g || seq > (UINT64_MAX - MC100_MAX_PCM_BYTES / 2) / 320)
    return MC100_INVALID;
  if (w->failed)
    return w->failed;
  if (w->active || !w->count)
    return MC100_NOT_READY;
  uint64_t total, available;
  mc100_result_t r = w->io.space(w->ctx, &total, &available);
  if (r)
    return fail(w, r);
  if (!total || available < floor_space(total))
    return MC100_FULL;
  w->generation = g;
  w->segment = 0;
  w->has_frames = false;
  w->checkpoint_ms = 0;
  return fail(w, claim(w, seq));
}
static mc100_result_t record(mc100_writer_t *w, mc100_index_validation_t *v,
                             uint16_t type, const uint8_t *payload,
                             uint32_t bytes, uint32_t reason) {
  mc100_index_record_t rec = {0};
  rec.type = type;
  rec.journal_seq = v->record_count;
  rec.pcm_offset = v->pcm_bytes;
  rec.first_source_sample = v->first_source_sample + v->pcm_bytes / 2;
  rec.generation = w->generation;
  rec.valid_bytes = bytes;
  if (bytes)
    rec.payload_crc32 = mc100_crc32(payload, bytes);
  if (type == MC100_INDEX_INCIDENT) {
    rec.flags = MC100_INDEX_INCOMPLETE;
    rec.detail = reason;
  }
  uint8_t encoded[64];
  mc100_result_t r = mc100_index_encode(encoded, &rec);
  if (r)
    return r;
  mc100_index_validation_t next = *v;
  r = mc100_index_validation_accept(&next, &rec);
  if (r)
    return r;
  r = write_exact(w, w->idx, 512 + (uint64_t)v->record_count * 64, encoded,
                  sizeof(encoded));
  if (!r)
    *v = next;
  return r;
}
static mc100_result_t flush(mc100_writer_t *w, bool tail, uint16_t terminal,
                            uint32_t reason) {
  size_t bytes = tail ? w->buffered : w->buffered / 4096 * 4096;
  if (!bytes && !terminal)
    return MC100_OK;
  mc100_index_validation_t v = w->validation;
  size_t records = (bytes + 4095) / 4096 + 1;
  if (records > MC100_INDEX_MAX_RECORDS - v.record_count)
    return MC100_FULL;
  mc100_result_t r;
  if (bytes) {
    r = write_exact(w, w->wav, 512 + v.pcm_bytes, w->staging, bytes);
    if (r)
      return r;
  }
  r = w->io.sync(w->ctx, w->wav);
  if (r)
    return r;
  for (size_t off = 0; off < bytes; off += 4096) {
    uint32_t count = (uint32_t)(bytes - off > 4096 ? 4096 : bytes - off);
    r = record(w, &v, MC100_INDEX_BLOCK, w->staging + off, count, 0);
    if (r)
      return r;
  }
  r = record(w, &v, terminal ? terminal : MC100_INDEX_CHECKPOINT, NULL, 0,
             reason);
  if (r)
    return r;
  r = w->io.sync(w->ctx, w->idx);
  if (r)
    return r;
  w->validation = v;
  w->buffered -= bytes;
  memmove(w->staging, w->staging + bytes, w->buffered);
  return MC100_OK;
}
static mc100_result_t finish(mc100_writer_t *w, uint32_t reason) {
  mc100_result_t r =
      flush(w, true, reason ? MC100_INDEX_INCIDENT : MC100_INDEX_FINAL, reason);
  if (r)
    return r;
  uint8_t header[512];
  r = mc100_wav_header(header, (uint32_t)w->validation.pcm_bytes);
  if (r)
    return r;
  r = w->io.truncate(w->ctx, w->wav, 512 + w->validation.pcm_bytes);
  if (r)
    return r;
  r = write_exact(w, w->wav, 0, header, sizeof(header));
  if (r)
    return r;
  r = w->io.sync(w->ctx, w->wav);
  if (r)
    return r;
  r = w->io.truncate(w->ctx, w->idx,
                     512 + (uint64_t)w->validation.record_count * 64);
  if (r)
    return r;
  r = w->io.sync(w->ctx, w->idx);
  if (r)
    return r;
  r = w->io.close(w->ctx, w->wav);
  if (r)
    return r;
  w->wav = NULL;
  r = w->io.close(w->ctx, w->idx);
  if (r)
    return r;
  w->idx = NULL;
  char wav[MC100_PATH_BYTES], idx[MC100_PATH_BYTES];
  size_t n = strlen(w->current.wav) - 9;
  const char *suffix = reason ? ".partial.wav" : ".wav";
  size_t tail = strlen(suffix);
  if (n >= sizeof(wav) - tail || n >= sizeof(idx) - 4)
    return MC100_INVALID;
  memcpy(wav, w->current.wav, n);
  memcpy(wav + n, suffix, tail + 1);
  memcpy(idx, w->current.idx, n);
  memcpy(idx + n, ".idx", 5);
  r = w->io.rename_no_replace(w->ctx, w->current.wav, wav);
  if (r)
    return r;
  r = w->io.rename_no_replace(w->ctx, w->current.idx, idx);
  if (r)
    return r;
  w->active = false;
  if (reason)
    w->reason = reason;
  return MC100_OK;
}
mc100_result_t mc100_writer_append(mc100_writer_t *w, const mc100_packet_t *p) {
  if (!w || !p)
    return MC100_INVALID;
  if (w->failed)
    return w->failed;
  if (!w->active || w->reason)
    return MC100_NOT_READY;
  if (p->generation != w->generation ||
      p->frame.seq > (UINT64_MAX - 320) / 320 ||
      (!w->has_frames && p->frame.seq != w->first_seq) ||
      (w->has_frames &&
       (w->last_seq == UINT64_MAX || p->frame.seq != w->last_seq + 1)))
    return MC100_INVALID;
  uint64_t total, available;
  mc100_result_t r = w->io.space(w->ctx, &total, &available);
  if (r)
    return fail(w, r);
  if (!total || available < floor_space(total)) {
    r = finish(w, MC100_INCIDENT_STORAGE_FULL);
    return fail(w, r ? r : MC100_FULL);
  }
  if (w->accepted == MC100_MAX_PCM_BYTES) {
    if (p->frame.seq > (UINT64_MAX - MC100_MAX_PCM_BYTES / 2) / 320 ||
        w->segment == UINT32_MAX)
      return MC100_INVALID;
    r = finish(w, 0);
    if (r)
      return fail(w, r);
    ++w->segment;
    r = claim(w, p->frame.seq);
    if (r)
      return fail(w, r);
  }
  /* Serialize explicitly: host and target endianness never changes PCM bytes.
   */
  for (size_t i = 0; i < 320; ++i) {
    uint16_t sample = (uint16_t)p->frame.pcm[i];
    w->staging[w->buffered++] = (uint8_t)sample;
    w->staging[w->buffered++] = (uint8_t)(sample >> 8);
    if (w->buffered == sizeof(w->staging)) {
      r = flush(w, false, 0, 0);
      if (r)
        return fail(w, r);
    }
  }
  w->accepted += 640;
  w->last_seq = p->frame.seq;
  w->has_frames = true;
  /* Keep at most active + one successor. After rotation, refill during early
   * RECORD instead of adding allocation latency to the boundary operation. */
  if (!w->count && w->accepted >= 50u * 640u) {
    r = make_slot(w);
    if (r) {
      if (r == MC100_FULL) {
        mc100_result_t ended = finish(w, MC100_INCIDENT_STORAGE_FULL);
        if (ended)
          r = ended;
      }
      return fail(w, r);
    }
  }
  return MC100_OK;
}
mc100_result_t mc100_writer_checkpoint(mc100_writer_t *w, uint64_t now) {
  if (!w)
    return MC100_INVALID;
  if (w->failed)
    return w->failed;
  if (!w->active)
    return MC100_NOT_READY;
  if (now < w->checkpoint_ms)
    return MC100_INVALID;
  if (now - w->checkpoint_ms < 1000)
    return MC100_OK;
  mc100_result_t r = flush(w, false, 0, 0);
  if (!r)
    w->checkpoint_ms = now;
  return fail(w, r);
}
mc100_result_t mc100_writer_close_through(mc100_writer_t *w,
                                          mc100_generation_t g, uint64_t seq,
                                          uint32_t reason) {
  if (!w || reason > MC100_INCIDENT_LOW_BAT_INTERRUPTED)
    return MC100_INVALID;
  if (w->failed)
    return w->failed;
  if (!w->active || !w->has_frames || g != w->generation || seq != w->last_seq)
    return MC100_INVALID;
  mc100_result_t r = finish(w, reason);
  /* A short final segment may close before its early-RECORD refill. Complete
   * that preparation inside CLOSE before the controller can announce LISTEN.
   * Abnormal close never initiates another allocation. */
  if (!r && !reason && !w->count)
    r = make_slot(w);
  return fail(w, r);
}
