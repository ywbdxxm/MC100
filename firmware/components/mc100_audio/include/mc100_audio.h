#ifndef MC100_AUDIO_H
#define MC100_AUDIO_H

#include "mc100_types.h"

typedef struct mc100_audio mc100_audio_t;

typedef struct {
    bool triggered;
    uint64_t first_live_seq;
    bool stopped;
    bool cutoff_valid;
    uint64_t last_accepted_seq;
    mc100_result_t error;
} mc100_audio_status_t;

typedef struct {
    uint16_t current;
    uint16_t peak;
    uint64_t drops;
    bool first_gap_valid;
    uint64_t first_gap_seq;
    mc100_generation_t first_gap_generation;
} mc100_audio_stats_t;

/* All calls require external serialization, including pop, snapshot reads and
 * status reads. This portable core provides content/ordering, not SPSC atomics.
 * T11 must supply bounded critical sections/publication. Never hold a lock over
 * storage I/O. create/destroy are startup/quiescent-only operations. */
mc100_audio_t *mc100_audio_create(void);
void mc100_audio_destroy(mc100_audio_t *a);
mc100_result_t mc100_audio_arm(mc100_audio_t *a, mc100_generation_t generation,
                              uint64_t min_seq);
/* trigger is the already-decided VAD edge/level (no VAD algorithm here).
 * When unarmed, one trigger is retained; the first eligible real frame after
 * ARM triggers even if its trigger argument is false. ARM requires one free
 * bank and one of two session slots, and changes nothing on NOT_READY.
 * Queue overflow, capture loss and invalid sequence continuity latch terminal
 * input errors until quiescent destroy/create; valid old data remains readable. */
mc100_result_t mc100_audio_push(mc100_audio_t *a, const mc100_frame_t *frame,
                               bool trigger);
mc100_result_t mc100_audio_snapshot(mc100_audio_t *a,
                                   mc100_generation_t generation,
                                   mc100_snapshot_t *snapshot);
mc100_result_t mc100_audio_snapshot_frame(mc100_audio_t *a,
                                         const mc100_snapshot_t *snapshot,
                                         uint16_t index, mc100_frame_t *frame);
mc100_result_t mc100_audio_pop(mc100_audio_t *a, mc100_packet_t *packet);
/* Read the FIFO head without consuming it.  Supervisor owners use this to
 * leave a successor generation queued while draining an older session. */
mc100_result_t mc100_audio_peek(const mc100_audio_t *a,
                               mc100_packet_t *packet);
mc100_result_t mc100_audio_stop(mc100_audio_t *a, mc100_generation_t generation,
                               uint64_t *last_accepted_seq);
/* STOP is idempotent. Always inspect cutoff_valid in status: an untriggered
 * grant has no cutoff, distinct from valid frame 0. A frozen nonempty snapshot
 * is accepted audio even when its first live packet cannot enter a full FIFO.
 * Runtime derives State TRIGGER's first written sequence from snapshot.first_seq
 * when count>0, otherwise from first_live_seq; publish a fault on status.error. */
mc100_result_t mc100_audio_status(mc100_audio_t *a, mc100_generation_t generation,
                                 mc100_audio_status_t *status);
mc100_result_t mc100_audio_stats(mc100_audio_t *a, mc100_audio_stats_t *stats);
/* Storage calls after copying every snapshot frame. Does not release session. */
mc100_result_t mc100_audio_snapshot_release(mc100_audio_t *a,
                                           mc100_generation_t generation);
/* CLOSED or explicit rejection/cancel: invalidates snapshot and discards only
 * this generation's queue packets. Caller must synchronize with consumer first;
 * release may compact the FIFO and must never race pop or snapshot_frame. */
mc100_result_t mc100_audio_release(mc100_audio_t *a,
                                  mc100_generation_t generation);
/* Driver-confirmed capture loss is terminal until destroy/create. */
mc100_result_t mc100_audio_capture_gap(mc100_audio_t *a, uint64_t first_gap_seq);

typedef mc100_result_t (*mc100_frame_callback_t)(void *, const mc100_frame_t *);
typedef struct {
    mc100_frame_t frame;
    size_t used_bytes;
    uint8_t low_byte;
    mc100_result_t error;
    bool sequence_exhausted;
} mc100_assembler_t;

void mc100_assembler_init(mc100_assembler_t *assembler, uint64_t initial_seq);
/* Input is signed 16-bit little-endian PCM. Odd final byte is retained.
 * Callback error latches: a partially consumed feed cannot be retried. Reset
 * explicitly with init; no frame is fabricated after a confirmed capture gap. */
mc100_result_t mc100_assembler_feed(mc100_assembler_t *assembler,
                                   const uint8_t *bytes, size_t length,
                                   mc100_frame_callback_t callback, void *context);
mc100_result_t mc100_assembler_capture_gap(mc100_assembler_t *assembler);

#endif
