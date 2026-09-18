#include "mc100_fake_io.h"
#include "mc100_writer.h"
#include <assert.h>
#include <string.h>
int main(void) {
  for (uint64_t frames = 15001; frames <= 30005; frames += 15004) {
    uint8_t id[16] = {3};
    mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
    mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, id);
    assert(mc100_writer_prepare(w) == MC100_OK);
    assert(mc100_writer_begin(w, 42, 100) == MC100_OK);
    mc100_packet_t p = {0};
    p.generation = 42;
    for (uint64_t i = 0; i < frames; ++i) {
      p.frame.seq = 100 + i;
      for (size_t j = 0; j < 320; ++j)
        p.frame.pcm[j] = (int16_t)((i * 320 + j) % 32767);
      size_t start = mc100_fake_io_log_count(f);
      assert(mc100_writer_append(w, &p) == MC100_OK);
      mc100_writer_status_t status;
      assert(mc100_writer_status(w, &status) == MC100_OK);
      assert(status.prepared_slots <= 1);
      if (i == 15000 || i == 30000) {
        for (size_t k = start; k < mc100_fake_io_log_count(f); ++k)
          assert(mc100_fake_io_log(f, k)->kind != 'A');
      }
    }
    assert(mc100_writer_close_through(w, 42, 99 + frames, 0) == MC100_OK);
    uint64_t seen = 0;
    for (size_t k = 0; k < mc100_fake_io_count(f); ++k) {
      const char *path = mc100_fake_io_path(f, k);
      size_t l = strlen(path), n;
      const uint8_t *b = mc100_fake_io_bytes(f, path, &n);
      if (l > 4 && !strcmp(path + l - 4, ".wav")) {
        uint64_t remain = (frames * 320 - seen);
        size_t samples = (n - 512) / 2;
        assert(samples == (remain > 4800000 ? 4800000 : (size_t)remain));
        for (size_t j = 0; j < samples; ++j) {
          uint16_t v = (uint16_t)((seen + j) % 32767);
          assert(b[512 + j * 2] == (uint8_t)v &&
                 b[513 + j * 2] == (uint8_t)(v >> 8));
        }
        seen += samples;
      }
    }
    assert(seen == frames * 320);
    mc100_writer_destroy(w);
    mc100_fake_io_destroy(f);
  }
  return 0;
}
