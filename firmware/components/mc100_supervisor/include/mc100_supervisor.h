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

typedef enum {
    MC100_SUPERVISOR_AUDIO_START = 1,
    MC100_SUPERVISOR_AUDIO_STOP = 2
} mc100_supervisor_audio_control_t;

typedef mc100_result_t (*mc100_supervisor_audio_control_fn)(
    void *ctx, mc100_supervisor_audio_control_t command,
    mc100_generation_t generation);

/* External monitor ingress is deliberately bounded.  The owner drains this
 * channel from its own task; producers never call State, Audio, or Storage
 * APIs directly.  Critical/fault intent uses a separate latch and therefore
 * cannot be rejected by a full normal queue. */
enum { MC100_SUPERVISOR_EVENT_QUEUE_CAPACITY = 8 };

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
    /* Optional short critical section around the fixed event channel.  On
     * target these may wrap a FreeRTOS portMUX; host tests may leave them
     * NULL or count calls.  They must not block or perform owner work. */
    void (*event_lock)(void *ctx);
    void (*event_unlock)(void *ctx);
    void *event_lock_ctx;
    /* Optional owner-task handshake.  NULL preserves host/legacy behavior. */
    mc100_supervisor_audio_control_fn audio_control;
    void *audio_control_ctx;
} mc100_supervisor_deps_t;

mc100_supervisor_t *mc100_supervisor_create(
    const mc100_supervisor_deps_t *deps);
void mc100_supervisor_destroy(mc100_supervisor_t *supervisor);
mc100_result_t mc100_supervisor_boot(mc100_supervisor_t *supervisor);
/* Recreate terminal Audio/Writer owner objects after the platform owner has
 * unmounted and remounted storage following LOW_BAT_HOLD recovery.  This is
 * valid only after EV_RECOVERED_POWER has moved State back to BOOT. */
mc100_result_t mc100_supervisor_rearm(mc100_supervisor_t *supervisor);
mc100_result_t mc100_supervisor_tick(mc100_supervisor_t *supervisor);
/* Called by monitor producers.  The event is copied before returning.  LOW,
 * TICK and other ordinary intents return MC100_FULL when the fixed queue is
 * full; CRITICAL and FAULT intents are retained by the non-droppable latch. */
mc100_result_t mc100_supervisor_post_event(mc100_supervisor_t *supervisor,
                                           const mc100_event_t *event);
/* Called by the supervisor owner (normally at the start of tick).  Critical
 * intent is dispatched before FIFO normal events. */
mc100_result_t mc100_supervisor_drain_events(mc100_supervisor_t *supervisor);
mc100_state_id_t mc100_supervisor_state(
    const mc100_supervisor_t *supervisor);
/* Read-only snapshot for diagnostics and host/target integration tests. The
 * supervisor remains the sole owner of the writer; callers must not retain or
 * mutate the returned structure. */
mc100_result_t mc100_supervisor_writer_status(
    const mc100_supervisor_t *supervisor, mc100_writer_status_t *status);

#endif
