#ifndef MC100_RECOVERY_H
#define MC100_RECOVERY_H

#include "mc100_io.h"

typedef struct {
  uint64_t valid_pcm_bytes;
  uint32_t recovered;
  uint32_t preserved;
  uint32_t invalid;
  uint32_t timed_out;
} mc100_recovery_report_t;

mc100_result_t mc100_recover(const mc100_io_t *io, void *ctx,
                             uint64_t deadline_ms,
                             uint64_t (*now_ms)(void *), void *clock_ctx,
                             mc100_recovery_report_t *report);

#endif
