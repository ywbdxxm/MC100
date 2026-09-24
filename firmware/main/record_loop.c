#include "record_loop.h"
#include "mc100_audio.h"
#include "mc100_platform.h"
#include "mc100_record_session.h"
#include "mc100_writer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>

enum {
    RECORD_QUEUE_FRAMES = 96,
    RECORD_PCM_BYTES = MC100_FRAME_SAMPLES * sizeof(int16_t),
    RECORD_CAPTURE_STACK_BYTES = 8192,
    RECORD_FIRST_FRAME_TIMEOUT_MS = 5000,
    RECORD_QUIESCE_TIMEOUT_MS = 2000
};

typedef struct {
    StaticQueue_t queue_control;
    _Alignas(portBYTE_ALIGNMENT)
        uint8_t queue_storage[RECORD_QUEUE_FRAMES * sizeof(mc100_frame_t)];
    QueueHandle_t frame_queue;
    TaskHandle_t storage_task, capture_task;
    portMUX_TYPE session_lock;
    /* Session, stop intent and first incident are protected by session_lock.
     * No FreeRTOS or platform calls occur inside that short critical section. */
    mc100_record_session_t session;
    bool stop_requested;
    uint32_t incident_reason;
    /* Capture owns these until it publishes producer_quiesced. */
    unsigned queue_peak, capture_stack_free;
    uint32_t overflows;
    /* Remaining fields belong only to the storage task. */
    mc100_writer_t *writer;
    bool mounted, writer_begun, writer_healthy;
    uint64_t capture_started_ms, written_frames, discarded_frames;
    uint32_t publications;
} record_runtime_t;

/* With external RAM disabled, zero-initialized static storage is internal
 * DRAM. Initialize only the lock at startup rather than putting the entire
 * 62 KiB queue into the image's initialized-data section. */
static record_runtime_t runtime;

static mc100_record_session_t session_snapshot(record_runtime_t *r)
{
    portENTER_CRITICAL(&r->session_lock);
    mc100_record_session_t session = r->session;
    portEXIT_CRITICAL(&r->session_lock);
    return session;
}

static void latch_fault(record_runtime_t *r, mc100_result_t fault,
                        uint32_t incident)
{
    portENTER_CRITICAL(&r->session_lock);
    if (!r->session.faulted) {
        mc100_record_session_mark_fault(&r->session, fault);
        r->incident_reason = incident;
    }
    r->stop_requested = true;
    portEXIT_CRITICAL(&r->session_lock);
    xTaskNotifyGive(r->storage_task);
}

static bool stopping(record_runtime_t *r)
{
    uint64_t now = mc100_platform_now_ms();
    portENTER_CRITICAL(&r->session_lock);
    bool expired = mc100_record_session_deadline_expired(&r->session, now);
    bool stopped = r->stop_requested;
    portEXIT_CRITICAL(&r->session_lock);
    if (expired) {
        latch_fault(r, MC100_TIMEOUT, MC100_INCIDENT_MIC_IO);
        return true;
    }
    return stopped;
}

static mc100_result_t enqueue_frame(void *context, const mc100_frame_t *frame)
{
    record_runtime_t *r = context;
    portENTER_CRITICAL(&r->session_lock);
    bool allowed = !r->stop_requested &&
        mc100_record_session_can_enqueue(&r->session);
    portEXIT_CRITICAL(&r->session_lock);
    if (!allowed) return MC100_NOT_READY;
    if (xQueueSend(r->frame_queue, frame, 0) != pdPASS) {
        latch_fault(r, MC100_FULL, MC100_INCIDENT_QUEUE_OVERFLOW);
        return MC100_FULL;
    }
    unsigned queued = (unsigned)uxQueueMessagesWaiting(r->frame_queue);
    if (queued > r->queue_peak) r->queue_peak = queued;
    uint64_t now = mc100_platform_now_ms();
    portENTER_CRITICAL(&r->session_lock);
    if (!r->session.started) mc100_record_session_start(&r->session, now);
    mc100_result_t result = mc100_record_session_note_enqueued(&r->session);
    if (mc100_record_session_target_reached(&r->session))
        r->stop_requested = true;
    portEXIT_CRITICAL(&r->session_lock);
    if (result != MC100_OK)
        latch_fault(r, result, MC100_INCIDENT_INTERNAL_PROTOCOL);
    /* Publish the accepted count before waking storage: storage only pops
     * counted frames, so it cannot consume the first frame before start(). */
    xTaskNotifyGive(r->storage_task);
    return result;
}

static void capture_task(void *context)
{
    record_runtime_t *r = context;
    mc100_assembler_t assembler;
    mc100_assembler_init(&assembler, 0);
    mc100_result_t result = mc100_platform_audio_start();
    if (result != MC100_OK) latch_fault(r, result, MC100_INCIDENT_MIC_IO);
    uint8_t pcm[RECORD_PCM_BYTES];
    while (result == MC100_OK && !stopping(r)) {
        size_t actual = 0;
        result = mc100_platform_audio_read(pcm, sizeof(pcm), &actual, 100);
        if (result != MC100_OK && result != MC100_TIMEOUT) {
            latch_fault(r, result, MC100_INCIDENT_MIC_IO);
            break;
        }
        /* A timeout may still carry a valid partial read. The assembler owns
         * odd bytes and partial frames; an empty timeout produces nothing. */
        if (actual != 0) {
            result = mc100_assembler_feed(&assembler, pcm, actual,
                                           enqueue_frame, r);
            if (result != MC100_OK) {
                latch_fault(r, result, MC100_INCIDENT_MIC_IO);
                break;
            }
        }
        result = MC100_OK;
    }
    result = mc100_platform_audio_stop();
    if (result != MC100_OK) latch_fault(r, result, MC100_INCIDENT_MIC_IO);
    r->overflows = mc100_platform_audio_overflows();
    r->capture_stack_free = (unsigned)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&r->session_lock);
    if (result == MC100_OK)
        mc100_record_session_mark_producer_quiesced(&r->session);
    portEXIT_CRITICAL(&r->session_lock);
    xTaskNotifyGive(r->storage_task);
    /* Keep the handle/owner valid even when driver teardown failed. */
    for (;;) (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void writer_fault(record_runtime_t *r, mc100_result_t result)
{
    r->writer_healthy = false;
    latch_fault(r, result, result == MC100_FULL ? MC100_INCIDENT_STORAGE_FULL :
                                                 MC100_INCIDENT_STORAGE_IO);
}

static void drain_publications(record_runtime_t *r)
{
    mc100_writer_publication_t publication;
    for (;;) {
        mc100_result_t result = mc100_writer_publication_pop(r->writer,
                                                             &publication);
        if (result == MC100_NOT_READY) return;
        if (result != MC100_OK) { writer_fault(r, result); return; }
        ++r->publications;
        printf("RECORDER_PUBLICATION generation=%" PRIu64 " segment=%" PRIu32
               " path=%s\n", publication.generation,
               publication.segment_index, publication.name);
    }
}

static void append_frame(record_runtime_t *r, const mc100_packet_t *packet)
{
    mc100_record_session_t session = session_snapshot(r);
    if (!r->writer_healthy || session.faulted) {
        ++r->discarded_frames;
        return;
    }
    mc100_result_t result;
    if (!r->writer_begun) {
        result = mc100_writer_begin(r->writer, 1, packet->frame.seq);
        if (result != MC100_OK) { writer_fault(r, result); return; }
        r->writer_begun = true;
    }
    /* begin and prior I/O may have allowed capture to report a fault. */
    (void)stopping(r);
    session = session_snapshot(r);
    if (session.faulted) { ++r->discarded_frames; return; }
    result = mc100_writer_append(r->writer, packet);
    /* Rotation can publish one segment before a later operation fails. */
    if (result != MC100_OK) writer_fault(r, result);
    drain_publications(r);
    if (result != MC100_OK) return;
    ++r->written_frames;
    portENTER_CRITICAL(&r->session_lock);
    result = mc100_record_session_note_consumed(&r->session);
    portEXIT_CRITICAL(&r->session_lock);
    if (result != MC100_OK) {
        /* A concurrent capture fault deliberately prevents clean accounting. */
        latch_fault(r, result, MC100_INCIDENT_INTERNAL_PROTOCOL);
        return;
    }
    (void)stopping(r);
    session = session_snapshot(r);
    if (!session.faulted && r->writer_healthy) {
        result = mc100_writer_checkpoint(r->writer, mc100_platform_now_ms());
        if (result != MC100_OK) writer_fault(r, result);
    }
}

static bool quiesce_capture(record_runtime_t *r)
{
    portENTER_CRITICAL(&r->session_lock);
    r->stop_requested = true;
    portEXIT_CRITICAL(&r->session_lock);
    xTaskNotifyGive(r->capture_task);
    uint64_t until = mc100_platform_now_ms() + RECORD_QUIESCE_TIMEOUT_MS;
    do {
        if (session_snapshot(r).producer_quiesced) return true;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    } while (mc100_platform_now_ms() < until);
    latch_fault(r, MC100_TIMEOUT, MC100_INCIDENT_MIC_IO);
    return false;
}

static void abandon_storage(record_runtime_t *r)
{
    /* Only used before capture starts or after its successful acknowledgement. */
    if (r->writer) {
        mc100_writer_abandon(r->writer, r->incident_reason);
        mc100_result_t result = mc100_writer_release_handles(r->writer);
        printf("RECORDER_RELEASE result=%d\n", result);
        if (result != MC100_OK) { writer_fault(r, result); return; }
        mc100_writer_destroy(r->writer);
        r->writer = NULL;
    }
    if (r->mounted) {
        mc100_result_t result = mc100_platform_storage_unmount();
        printf("RECORDER_UNMOUNT result=%d\n", result);
        if (result == MC100_OK) r->mounted = false;
        else latch_fault(r, result, MC100_INCIDENT_STORAGE_IO);
    }
}

static void finalize(record_runtime_t *r)
{
    (void)stopping(r);
    mc100_record_session_t session = session_snapshot(r);
    mc100_writer_status_t status;
    mc100_result_t result = mc100_writer_status(r->writer, &status);
    if (result != MC100_OK) {
        writer_fault(r, result);
        abandon_storage(r);
        return;
    }
    bool clean = mc100_record_session_can_clean_close(&session) &&
        r->writer_healthy && status.active && status.has_frames &&
        !status.latched_reason && status.generation == 1 &&
        status.last_seq == session.consumed_frames - 1 &&
        r->written_frames == session.consumed_frames;
    if (!clean && !session.faulted)
        latch_fault(r, MC100_CORRUPT, MC100_INCIDENT_INTERNAL_PROTOCOL);
    if (status.active && status.has_frames) {
        result = mc100_writer_close_through(r->writer, 1, status.last_seq,
                                            clean ? 0 : r->incident_reason);
        printf("RECORDER_CLOSE result=%d clean=%u last_seq=%" PRIu64 "\n",
               result, (unsigned)(clean && result == MC100_OK), status.last_seq);
        if (result != MC100_OK) writer_fault(r, result);
        drain_publications(r);
        if (result == MC100_OK && r->writer_healthy) return;
    }
    abandon_storage(r);
}

static void idle(record_runtime_t *r, bool reset_required)
{
    mc100_record_session_t session = session_snapshot(r);
    /* Capture metrics are read only after its release/acquire acknowledgement. */
    printf("RECORDER_STOP enqueued=%" PRIu64 " consumed=%" PRIu64
           " written=%" PRIu64 " discarded=%" PRIu64
           " queue_peak=%u overflow=%" PRIu32 " fault=%d reason=%" PRIu32
           " publications=%" PRIu32 " quiesced=%u mounted=%u"
           " capture_stack_free=%u storage_stack_free=%u heap=%u"
           " reset_required=%u\n",
           session.enqueued_frames, session.consumed_frames, r->written_frames,
           r->discarded_frames, session.producer_quiesced ? r->queue_peak : 0,
           session.producer_quiesced ? r->overflows : 0, session.fault,
           r->incident_reason, r->publications,
           (unsigned)session.producer_quiesced, (unsigned)r->mounted,
           session.producer_quiesced ? r->capture_stack_free : 0,
           (unsigned)uxTaskGetStackHighWaterMark(NULL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)reset_required);
    puts("RECORDER state=IDLE");
    fflush(stdout);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void mc100_record_run(void)
{
    record_runtime_t *r = &runtime;
    portMUX_INITIALIZE(&r->session_lock);
    r->storage_task = xTaskGetCurrentTaskHandle();
    mc100_record_session_init(&r->session);
    printf("RECORDER_BOOT target_frames=%d queue_frames=%d queue_bytes=%u"
           " runtime_bytes=%u capture_stack_bytes=%d storage_stack_bytes=%d\n",
           MC100_RECORD_TARGET_FRAMES, RECORD_QUEUE_FRAMES,
           (unsigned)sizeof(r->queue_storage), (unsigned)sizeof(*r),
           RECORD_CAPTURE_STACK_BYTES, MC100_RECORD_STORAGE_STACK_BYTES);
    mc100_result_t result = mc100_platform_board_init();
    printf("RECORDER_BOARD result=%d\n", result);
    if (result != MC100_OK) {
        latch_fault(r, result, MC100_INCIDENT_CONFIG_INVALID);
        idle(r, false);
    }
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        result = mc100_platform_storage_mount();
        printf("RECORDER_MOUNT attempt=%u result=%d\n", attempt, result);
        if (result == MC100_OK) { r->mounted = true; break; }
        if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!r->mounted) {
        latch_fault(r, result, MC100_INCIDENT_STORAGE_IO);
        idle(r, false);
    }
    uint8_t boot_id[16];
    esp_fill_random(boot_id, sizeof(boot_id));
    r->writer = mc100_writer_create(mc100_platform_storage_io(),
                                    mc100_platform_storage_context(), boot_id);
    uint64_t preparing = mc100_platform_now_ms();
    result = r->writer ? mc100_writer_prepare(r->writer) : MC100_FULL;
    printf("RECORDER_PREPARE result=%d elapsed_ms=%" PRIu64 "\n",
           result, mc100_platform_now_ms() - preparing);
    if (result != MC100_OK) {
        writer_fault(r, result);
        abandon_storage(r);
        idle(r, false);
    }
    r->writer_healthy = true;
    r->frame_queue = xQueueCreateStatic(RECORD_QUEUE_FRAMES,
        sizeof(mc100_frame_t), r->queue_storage, &r->queue_control);
    r->capture_started_ms = mc100_platform_now_ms();
    if (!r->frame_queue ||
        xTaskCreatePinnedToCore(capture_task, "mc100_capture",
            RECORD_CAPTURE_STACK_BYTES, r, 20, &r->capture_task, 1) != pdPASS) {
        latch_fault(r, MC100_FULL, MC100_INCIDENT_INTERNAL_PROTOCOL);
        abandon_storage(r);
        idle(r, false);
    }
    puts("RECORDER state=RECORDING");
    /* This stack wrapper is the existing writer API seam. The queue itself
     * contains only mc100_frame_t, with no second persistent PCM buffer. */
    mc100_packet_t packet = {.generation = 1};
    while (!stopping(r)) {
        mc100_record_session_t session = session_snapshot(r);
        if (!session.started && mc100_platform_now_ms() - r->capture_started_ms >=
                                RECORD_FIRST_FRAME_TIMEOUT_MS) {
            latch_fault(r, MC100_TIMEOUT, MC100_INCIDENT_MIC_IO);
            break;
        }
        if (session.enqueued_frames > session.consumed_frames &&
            xQueueReceive(r->frame_queue, &packet.frame, 0) == pdPASS)
            append_frame(r, &packet);
        else
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    if (!quiesce_capture(r)) {
        /* No drain, close, abandon, release or unmount without acknowledgement. */
        idle(r, true);
    }
    while (xQueueReceive(r->frame_queue, &packet.frame, 0) == pdPASS)
        append_frame(r, &packet);
    finalize(r);
    idle(r, false);
}
