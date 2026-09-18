#include "mc100_fake_io.h"
#include "mc100_writer.h"
#include <assert.h>
#include <stdint.h>
int main(void) {
  assert(!mc100_space_can_prepare(1000000000, 109863168, 9863168));
  assert(mc100_space_can_prepare(1000000000, 110911744, 9863168));
  assert(!mc100_space_can_prepare(1000000001, 110911744, 9863168));
  assert(!mc100_space_can_prepare(UINT64_MAX, UINT64_MAX, UINT64_MAX));
  uint8_t id[16] = {7};
  mc100_fake_io_t *f = mc100_fake_io_create(1000000001);
  mc100_writer_t *w = mc100_writer_create(mc100_fake_io_ops(), f, id);
  assert(mc100_writer_prepare(w) == MC100_OK);
  mc100_fake_io_free_bytes(f, 100000000);
  assert(mc100_writer_begin(w, 1, 0) == MC100_FULL);
  mc100_fake_io_free_bytes(f, 100000001);
  assert(mc100_writer_begin(w, 1, 0) == MC100_OK);
  mc100_writer_abandon(w, 9);
  assert(mc100_writer_release_handles(w) == MC100_OK);
  mc100_writer_destroy(w);
  mc100_fake_io_destroy(f);
  return 0;
}
