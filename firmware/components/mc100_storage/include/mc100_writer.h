#ifndef MC100_WRITER_H
#define MC100_WRITER_H
#include "mc100_format.h"
#include "mc100_io.h"
enum {
  MC100_STAGING_BYTES = 32768,
  MC100_SLOT_BYTES = 9863168,
  /* Publication is a hand-off queue, not an unbounded event log.  The
   * supervisor drains it after each writer operation; a full queue fails the
   * next finalization before any rename, preserving a one-to-one hand-off for
   * every clean publication committed by this writer instance. */
  MC100_WRITER_PUBLICATION_CAPACITY = 4
};
typedef struct mc100_writer mc100_writer_t;
typedef struct {
  /* Writer-owned, durably published WAV filename (never a .part path). */
  char name[MC100_PATH_BYTES];
  mc100_generation_t generation;
  uint32_t segment_index;
} mc100_writer_publication_t;
typedef struct {
  uint32_t prepared_slots, segment_index, latched_reason;
  uint64_t accepted_bytes, committed_bytes, last_seq;
  mc100_generation_t generation;
  bool active, has_frames, abandoned;
} mc100_writer_status_t;
bool mc100_space_can_prepare(uint64_t total, uint64_t free_bytes,
                             uint64_t reserve_bytes);
mc100_writer_t *mc100_writer_create(const mc100_io_t *, void *,
                                    const uint8_t boot_id[16]);
/* Neither destroy nor abandon issues any I/O. Quiesced adapter owns remaining
 * handles after abandon; runtime must release them at unmount. */
void mc100_writer_destroy(mc100_writer_t *);
void mc100_writer_abandon(mc100_writer_t *, uint32_t reason);
/* Owning task only after in-flight I/O is quiescent; invokes adapter's no-write
 * close for retained handles. Separate from abandon/destroy to honor power
 * hold. */
mc100_result_t mc100_writer_release_handles(mc100_writer_t *);
mc100_result_t mc100_writer_prepare(mc100_writer_t *);
mc100_result_t mc100_writer_begin(mc100_writer_t *, mc100_generation_t,
                                  uint64_t first_seq);
mc100_result_t mc100_writer_append(mc100_writer_t *, const mc100_packet_t *);
mc100_result_t mc100_writer_checkpoint(mc100_writer_t *, uint64_t now_ms);
mc100_result_t mc100_writer_close_through(mc100_writer_t *, mc100_generation_t,
                                          uint64_t last_seq, uint32_t reason);
mc100_result_t mc100_writer_status(const mc100_writer_t *,
                                   mc100_writer_status_t *);
/* Remove the oldest durable publication record.  MC100_NOT_READY means the
 * queue is empty.  The returned name remains valid in the caller-owned record
 * after the writer advances its queue. */
mc100_result_t mc100_writer_publication_pop(
    mc100_writer_t *, mc100_writer_publication_t *);
#endif
