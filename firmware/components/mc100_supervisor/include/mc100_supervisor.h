#ifndef MC100_SUPERVISOR_H
#define MC100_SUPERVISOR_H

#include "mc100_audio.h"
#include "mc100_io.h"
#include "mc100_recovery.h"
#include "mc100_state.h"
#include "mc100_upload.h"
#include "mc100_vad.h"
#include "mc100_writer.h"

typedef struct mc100_supervisor mc100_supervisor_t;

typedef struct {
    const mc100_io_t *io;
    void *io_ctx;
    uint64_t (*now_ms)(void *);
    void *clock_ctx;
    bool (*battery_ready)(void *);
    void *battery_ctx;
    bool (*driver_ready)(void *);
    void *driver_ctx;
    mc100_result_t (*pcm_read)(void *ctx, uint8_t *buf, size_t cap,
                               size_t *count, uint32_t timeout_ms);
    void *pcm_ctx;
    mc100_vad_t vad;
    mc100_upload_sink_t upload;
    const uint8_t *boot_id;
} mc100_supervisor_deps_t;

mc100_supervisor_t *mc100_supervisor_create(
    const mc100_supervisor_deps_t *deps);
void mc100_supervisor_destroy(mc100_supervisor_t *supervisor);
mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *supervisor);
mc100_result_t mc100_supervisor_tick(mc100_supervisor_t *supervisor);
mc100_state_id_t mc100_supervisor_state(
    const mc100_supervisor_t *supervisor);
/* Read-only snapshot for diagnostics and host/target integration tests. The
 * supervisor remains the sole owner of the writer; callers must not retain or
 * mutate the returned structure. */
mc100_result_t mc100_supervisor_writer_status(
    const mc100_supervisor_t *supervisor, mc100_writer_status_t *status);

#endif
