#include "mc100_fake_io.h"
#include "mc100_writer.h"
#include <assert.h>
#include <string.h>
#if defined(_WIN32) && defined(MC100_TEST_FILE_IO)
#define WIN32_LEAN_AND_MEAN
#include "mc100_file_io.h"
#include <windows.h>
static void native_files(const char *directory) {
  mc100_file_io_t *f = mc100_file_io_create(directory);
  assert(f);
  const mc100_io_t *io = mc100_file_io_ops();
  uint8_t id[16] = {6};
  ULONGLONG tick = GetTickCount64();
  DWORD pid = GetCurrentProcessId();
  memcpy(id + 1, &tick, sizeof(tick));
  memcpy(id + 9, &pid, sizeof(pid));
  mc100_writer_t *w = mc100_writer_create(io, f, id);
  assert(w);
  assert(mc100_writer_prepare(w) == MC100_OK);
  assert(mc100_writer_begin(w, 9, 100) == MC100_OK);
  mc100_packet_t p = {0};
  p.generation = 9;
  for (uint64_t i = 100; i < 108; ++i) {
    p.frame.seq = i;
    for (size_t j = 0; j < 320; ++j)
      p.frame.pcm[j] = (int16_t)(i + j);
    assert(mc100_writer_append(w, &p) == MC100_OK);
  }
  assert(mc100_writer_close_through(w, 9, 107, 0) == MC100_OK);
  static const char hex[] = "0123456789abcdef";
  char name[MC100_PATH_BYTES];
  for (size_t i = 0; i < 16; ++i) {
    name[i * 2] = hex[id[i] >> 4];
    name[i * 2 + 1] = hex[id[i] & 15];
  }
  memcpy(name + 32, "_9_0.wav", 9);
  uint64_t size = 0;
  assert(io->stat(f, name, &size) == MC100_OK && size == 5632);
  mc100_file_t file = NULL;
  assert(io->open_exclusive(f, name, &file) == MC100_NOT_READY);
  assert(io->open_read(f, name, &file) == MC100_OK);
  uint8_t data[640];
  size_t actual = 0;
  assert(io->read_at(f, file, 512, data, sizeof(data), &actual) == MC100_OK &&
         actual == 640);
  for (size_t j = 0; j < 320; ++j) {
    uint16_t s = (uint16_t)(100 + j);
    assert(data[j * 2] == (uint8_t)s && data[j * 2 + 1] == (uint8_t)(s >> 8));
  }
  assert(io->write_at(f, file, 0, data, 1, &actual) == MC100_INVALID);
  assert(io->close(f, file) == MC100_OK);
  mc100_writer_destroy(w);
  mc100_file_io_destroy(f);
}
#endif
static void successive_sessions(void) {
  uint8_t id[16] = {4};
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, id);
  assert(mc100_writer_prepare(w) == MC100_OK);
  for (uint64_t generation = 1; generation <= 4; ++generation) {
    assert(mc100_writer_begin(w, generation, 0) == MC100_OK);
    mc100_packet_t p = {0};
    p.generation = generation;
    assert(mc100_writer_append(w, &p) == MC100_OK);
    assert(mc100_writer_close_through(w, generation, 0, 0) == MC100_OK);
    mc100_writer_status_t s;
    assert(mc100_writer_status(w, &s) == MC100_OK);
    assert(s.prepared_slots >= 1);
  }
  size_t before = mc100_fake_io_log_count(f);
  assert(mc100_writer_begin(w, 1, 0) == MC100_NOT_READY);
  for (size_t i = before; i < mc100_fake_io_log_count(f); ++i) {
    char kind = mc100_fake_io_log(f, i)->kind;
    assert(kind != 'W' && kind != 'N' && kind != 'A' && kind != 'X');
  }
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
}
int main(int argc, char **argv) {
#if defined(_WIN32) && defined(MC100_TEST_FILE_IO)
  if (argc == 2)
    native_files(argv[1]);
#else
  (void)argc;
  (void)argv;
#endif
  successive_sessions();
  uint8_t boot[16] = {1};
  mc100_fake_io_t *f = mc100_fake_io_create(1000000000);
  mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, boot);
  assert(w);
  assert(mc100_writer_prepare(w) == MC100_OK);
  assert(mc100_fake_io_count(f) == 4);
  mc100_writer_destroy(w);
  for (unsigned i = 0; i < 1000; ++i) {
    boot[1] = (uint8_t)i;
    w = mc100_writer_create(mc100_fake_io_ops(), f, boot);
    assert(mc100_writer_prepare(w) == MC100_OK);
    mc100_writer_destroy(w);
  }
  assert(mc100_fake_io_count(f) == 4);
  w = mc100_writer_create(mc100_fake_io_ops(), f, boot);
  assert(mc100_writer_prepare(w) == MC100_OK);
  assert(mc100_writer_begin(w, 7, 12) == MC100_OK);
  mc100_packet_t p = {0};
  p.generation = 7;
  for (uint64_t i = 12; i < 115; ++i) {
    p.frame.seq = i;
    for (size_t j = 0; j < 320; ++j)
      p.frame.pcm[j] = (int16_t)(i + j);
    assert(mc100_writer_append(w, &p) == MC100_OK);
    if (i == 40)
      assert(mc100_writer_checkpoint(w, 1000) == MC100_OK);
  }
  assert(mc100_writer_close_through(w, 7, 113, 0) == MC100_INVALID);
  assert(mc100_writer_close_through(w, 7, 114, 0) == MC100_OK);
  bool found = false;
  for (size_t k = 0; k < mc100_fake_io_count(f); ++k) {
    const char *path = mc100_fake_io_path(f, k);
    size_t n;
    const uint8_t *b = mc100_fake_io_bytes(f, path, &n);
    size_t l = strlen(path);
    if (l > 4 && !strcmp(path + l - 4, ".wav")) {
      assert(n == 512 + 103 * 640);
      for (size_t j = 0; j < 103 * 320; ++j) {
        uint16_t v = (uint16_t)(12 + j / 320 + j % 320);
        assert(b[512 + j * 2] == (uint8_t)v);
        assert(b[513 + j * 2] == (uint8_t)(v >> 8));
      }
      found = true;
    }
  }
  assert(found);
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
  return 0;
}
