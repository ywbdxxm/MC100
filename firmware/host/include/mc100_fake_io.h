#ifndef MC100_FAKE_IO_H
#define MC100_FAKE_IO_H
#include "mc100_io.h"
typedef struct mc100_fake_io mc100_fake_io_t;
typedef struct {
  char kind;
  char path[MC100_PATH_BYTES];
  uint64_t offset;
  size_t length;
} mc100_fake_op_t;
mc100_fake_io_t *mc100_fake_io_create(uint64_t total);
void mc100_fake_io_destroy(mc100_fake_io_t *);
const mc100_io_t *mc100_fake_io_ops(void);
void mc100_fake_io_fault(mc100_fake_io_t *, size_t relative_operation,
                         mc100_result_t result, bool short_transfer);
void mc100_fake_io_free_bytes(mc100_fake_io_t *, uint64_t);
size_t mc100_fake_io_count(const mc100_fake_io_t *);
size_t mc100_fake_io_log_count(const mc100_fake_io_t *);
const mc100_fake_op_t *mc100_fake_io_log(const mc100_fake_io_t *, size_t);
const uint8_t *mc100_fake_io_bytes(const mc100_fake_io_t *, const char *,
                                   size_t *);
const char *mc100_fake_io_path(const mc100_fake_io_t *, size_t);
#endif
