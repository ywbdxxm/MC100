#ifndef MC100_STATE_H
#define MC100_STATE_H

#include "mc100_types.h"

typedef struct mc100_state mc100_state_t;
typedef enum { MC100_BOOT, MC100_LISTEN, MC100_RECORD, MC100_LOW_BAT,
    MC100_LOW_BAT_HOLD, MC100_FAULT } mc100_state_id_t;
typedef enum { MC100_EV_READY, MC100_EV_TRIGGER, MC100_EV_OPENED,
    MC100_EV_SILENCE_END, MC100_EV_CAPTURE_STOPPED, MC100_EV_CLOSED,
    MC100_EV_LOW, MC100_EV_CRITICAL, MC100_EV_RECOVERED_POWER,
    MC100_EV_FAULT, MC100_EV_TICK, MC100_EV_ROTATED,
    MC100_EV_ARMED, MC100_EV_HELD } mc100_event_id_t;
typedef struct {
    mc100_event_id_t id;
    mc100_generation_t generation;
    uint64_t seq, now_ms;
    uint32_t detail;
    bool seq_valid;
    bool global;
} mc100_event_t;
typedef enum { MC100_ACT_ARM, MC100_ACT_OPEN, MC100_ACT_STOP_CAPTURE,
    MC100_ACT_CLOSE_THROUGH, MC100_ACT_RELEASE, MC100_ACT_HOLD,
    MC100_ACT_BOOT, MC100_ACT_REPORT_FAULT } mc100_action_id_t;
typedef struct {
    mc100_action_id_t id;
    mc100_generation_t generation;
    uint64_t seq;
    uint32_t detail;
    bool seq_valid;
} mc100_action_t;

/* Stable diagnostic codes; zero is not a fault. */
typedef enum {
    MC100_FAULT_MIC_IO = 1, MC100_FAULT_STORAGE_IO = 2,
    MC100_FAULT_STORAGE_FULL = 3, MC100_FAULT_QUEUE_OVERFLOW = 4,
    MC100_FAULT_ADC_INVALID = 5, MC100_FAULT_CONFIG_INVALID = 6,
    MC100_FAULT_RECOVERY_REQUIRED = 7, MC100_FAULT_INTERNAL_PROTOCOL = 8,
    MC100_FAULT_CONTROL_TIMEOUT = 9, MC100_FAULT_STORAGE_TIMEOUT = 10
} mc100_fault_reason_t;
typedef enum {
    MC100_HOLD_LOW = 1,
    MC100_HOLD_CRITICAL_NO_WRITES = 2,
    MC100_HOLD_FAULT_NO_WRITES = 3
} mc100_hold_reason_t;
enum { MC100_STATE_ACTION_CAPACITY = 8, MC100_CONTROL_TIMEOUT_MS = 200,
    MC100_STORAGE_TIMEOUT_MS = 1500, MC100_ARM_SILENCE_FRAMES = 750 };

/* Single State owner only. Allocate at startup; destroy only after all owners
 * have joined. step allocates nothing and commits state/actions atomically.
 * Caller provides all 8 action slots. Invalid input has no side effects.
 * READY(detail >= 2) is sent only AFTER battery stability, recovery and driver
 * readiness. Runtime must check battery_is_stable before BOOT writes.
 * ARM.detail preauthorizes the 750-frame silence stop. ARM.seq (when valid) is
 * the minimum first sequence. ARMED must precede TRIGGER for the granted token.
 * TRIGGER.seq is the snapshot first sequence, not the VAD trigger sequence.
 * CAPTURE_STOPPED.seq_valid=false means no accepted data, including seq 0.
 * CLOSE_THROUGH preserves that flag. The closing generation's valid queue must
 * remain drainable until CLOSED. A canceled pending generation may be discarded
 * only on its RELEASE, without removing packets belonging to the older session.
 * FAULT.global=true explicitly scopes a fault to the device; otherwise its
 * generation must match a current grant/session. Generation 0 is never granted
 * for recording; it is reserved for emergency HOLD if the serial is exhausted.
 * HOLD requests ALL owners quiesce/unmount; HELD with its command token is legal
 * only after no driver or queued action references any session buffer. Critical
 * and fault reasons prohibit new storage writes, including metadata writes.
 * RELEASE follows CLOSED, pending CAPTURE_STOPPED, or all-owner HELD; it never
 * authorizes cross-thread freeing of buffers still referenced by a driver.
 * ACK deadlines use elapsed time, inclusive at 200/1500 ms; duplicates cannot
 * refresh them. Provide monotonic time and TICK even while waiting for drivers.
 */
mc100_state_t *mc100_state_create(void);
void mc100_state_destroy(mc100_state_t *state);
mc100_result_t mc100_state_step(mc100_state_t *state,
    const mc100_event_t *event, mc100_action_t out[8], size_t *count);
mc100_state_id_t mc100_state_get(const mc100_state_t *state);

#endif
