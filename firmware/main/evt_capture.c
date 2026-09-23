#include "evt_capture.h"
#include "evt_commands.h"
#include "mc100_audio.h"
#include "mc100_platform.h"
#include "mc100_writer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* ============================================================================
 * EVT_USB_BENCH — 手动 USB 台架诊断工具，不是产品固件。
 * 产品自主录音循环（BOOT/LISTEN/VAD/RECORD 状态机）见 mc100_supervisor 组件
 * 与 product_runtime.c；本文件保留作驱动验证证据与 EVT 诊断，经构建开关启用。
 * This is a deliberately bounded USB bench path, not LISTEN/VAD or a battery
 * safety state machine. The storage task alone owns mount, writer, and files.
 * The persistent audio task alone owns all I2S lifecycle operations.
 * ========================================================================== */
static portMUX_TYPE core_lock = portMUX_INITIALIZER_UNLOCKED;
static mc100_audio_t *audio;
static mc100_writer_t *writer;
static TaskHandle_t audio_task, storage_task;
static mc100_generation_t generation;
static uint64_t next_seq, first_seq, final_seq;
static bool stop_requested, audio_done, reset_required, mounted, quiescence_failed;
static mc100_result_t audio_result, audio_stop_result;
static uint32_t overflow_count;
static char boot_text[33];
static uint64_t written_frames, energy;
static int16_t sample_min, sample_max;

static mc100_result_t publish_frame(void *context, const mc100_frame_t *frame)
{
    (void)context;
    portENTER_CRITICAL(&core_lock);
    mc100_result_t r = mc100_audio_push(audio, frame, frame->seq == first_seq + MC100_PREROLL_FRAMES);
    if (r == MC100_OK) next_seq = frame->seq + 1;
    if (r != MC100_OK || frame->seq == final_seq) {
        uint64_t cutoff;
        (void)mc100_audio_stop(audio, generation, &cutoff);
        stop_requested = true;
    }
    portEXIT_CRITICAL(&core_lock);
    xTaskNotifyGive(storage_task);
    return r;
}

static void audio_worker(void *context)
{
    (void)context;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        mc100_assembler_t assembler;
        mc100_assembler_init(&assembler, first_seq);
        mc100_result_t result = mc100_platform_audio_start();
        uint8_t pcm[640];
        uint64_t last_progress = mc100_platform_now_ms();
        while (result == MC100_OK) {
            portENTER_CRITICAL(&core_lock);
            bool stop = stop_requested;
            portEXIT_CRITICAL(&core_lock);
            if (stop) break;
            size_t count = 0;
            mc100_result_t r = mc100_platform_audio_read(pcm, sizeof(pcm), &count, 100);
            if (r != MC100_OK && r != MC100_TIMEOUT) { result = r; break; }
            if (count) {
                result = mc100_assembler_feed(&assembler, pcm, count, publish_frame, NULL);
                last_progress = mc100_platform_now_ms();
            } else if (mc100_platform_now_ms() - last_progress >= 2000) result = MC100_TIMEOUT;
        }
        mc100_result_t stopped = mc100_platform_audio_stop();
        if (stopped != MC100_OK && result == MC100_OK) result = stopped;
        uint32_t overflows = mc100_platform_audio_overflows();
        portENTER_CRITICAL(&core_lock);
        if (result != MC100_OK && result != MC100_FULL) (void)mc100_audio_capture_gap(audio, next_seq);
        uint64_t cutoff;
        (void)mc100_audio_stop(audio, generation, &cutoff);
        audio_result = result;
        audio_stop_result = stopped;
        overflow_count = overflows;
        audio_done = true;
        portEXIT_CRITICAL(&core_lock);
        xTaskNotifyGive(storage_task);
        /* Completion is a quiescence acknowledgement. No session state or
         * driver is touched again until the next start notification. */
    }
}

static mc100_result_t append_packet(const mc100_packet_t *packet, bool storage_enabled)
{
    mc100_result_t r = storage_enabled ? mc100_writer_append(writer, packet) : MC100_OK;
    if (r != MC100_OK) return r;
    ++written_frames;
    for (unsigned i = 0; i < MC100_FRAME_SAMPLES; ++i) {
        int32_t value = packet->frame.pcm[i];
        if (value < sample_min) sample_min = (int16_t)value;
        if (value > sample_max) sample_max = (int16_t)value;
        energy += (uint64_t)((int64_t)value * value);
    }
    return MC100_OK;
}

static void stop_intent(void)
{
    portENTER_CRITICAL(&core_lock);
    stop_requested = true;
    portEXIT_CRITICAL(&core_lock);
}

static bool worker_state(mc100_result_t *result)
{
    portENTER_CRITICAL(&core_lock);
    bool done = audio_done;
    *result = audio_result;
    portEXIT_CRITICAL(&core_lock);
    return done;
}

static bool join_audio(void)
{
    uint64_t deadline = mc100_platform_now_ms() + 5000;
    mc100_result_t result;
    while (!worker_state(&result)) {
        if (mc100_platform_now_ms() >= deadline) return false;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    portENTER_CRITICAL(&core_lock);
    bool quiescent = mc100_evt_safe_to_release(audio_done, audio_stop_result);
    portEXIT_CRITICAL(&core_lock);
    return quiescent;
}

static void abandon_storage(uint32_t reason)
{
    reset_required = true;
    mc100_writer_abandon(writer, reason);
    mc100_result_t released = mc100_writer_release_handles(writer);
    mc100_result_t unmounted = released == MC100_OK ? mc100_platform_storage_unmount() : released;
    if (unmounted == MC100_OK) mounted = false;
    printf("FAULT reset_required=1 release=%d unmount=%d\n", released, unmounted);
}

static void capture_seconds(uint32_t seconds, bool storage_enabled)
{
    const char *operation = storage_enabled ? "RECORD" : "CAPTURE";
    uint64_t frames, cutoff;
    if (!mc100_evt_can_start(storage_enabled, audio && audio_task, reset_required, mounted && writer) ||
        generation == UINT64_MAX || (!storage_enabled && seconds > 60) ||
        !mc100_evt_duration(seconds, next_seq, &frames, &cutoff)) {
        printf("%s result=2 reset_required=%u storage_ready=%u\n", operation,
               (unsigned)reset_required, (unsigned)(mounted && writer)); return;
    }
    printf("PREPARE seconds=%" PRIu32 " mode=EVT_USB_BENCH storage_enabled=%u\n", seconds, (unsigned)storage_enabled);
    fflush(stdout);
    mc100_result_t r = storage_enabled ? mc100_writer_prepare(writer) : MC100_OK;
    if (r != MC100_OK) {
        abandon_storage(r == MC100_FULL ? MC100_INCIDENT_STORAGE_FULL : MC100_INCIDENT_STORAGE_IO);
        printf("RECORD result=%d stage=prepare\n", r); return;
    }
    ++generation;
    first_seq = next_seq;
    final_seq = cutoff;
    written_frames = energy = 0;
    sample_min = INT16_MAX; sample_max = INT16_MIN;
    portENTER_CRITICAL(&core_lock);
    r = mc100_audio_arm(audio, generation, first_seq);
    stop_requested = false; audio_done = false; audio_result = MC100_OK; audio_stop_result = MC100_NOT_READY; overflow_count = 0;
    portEXIT_CRITICAL(&core_lock);
    if (r != MC100_OK) { reset_required = true; printf("%s result=%d stage=arm\n", operation, r); return; }
    uint64_t started = mc100_platform_now_ms(), deadline = started + (uint64_t)seconds * 1000 + 15000;
    bool began = false, snapshot_copied = false;
    mc100_packet_t packet = {.generation = generation};
    xTaskNotifyGive(audio_task);
    for (;;) {
        mc100_result_t capture_result;
        bool done = worker_state(&capture_result);
        if (!snapshot_copied) {
            mc100_snapshot_t snapshot;
            portENTER_CRITICAL(&core_lock);
            mc100_result_t available = mc100_audio_snapshot(audio, generation, &snapshot);
            portEXIT_CRITICAL(&core_lock);
            if (available == MC100_OK) {
                if (snapshot.count != MC100_PREROLL_FRAMES || snapshot.first_seq != first_seq) { r = MC100_CORRUPT; break; }
                r = storage_enabled ? mc100_writer_begin(writer, generation, snapshot.first_seq) : MC100_OK;
                if (r != MC100_OK) break;
                began = true;
                for (uint16_t i = 0; i < snapshot.count; ++i) {
                    portENTER_CRITICAL(&core_lock);
                    r = mc100_audio_snapshot_frame(audio, &snapshot, i, &packet.frame);
                    portEXIT_CRITICAL(&core_lock);
                    if (r == MC100_OK) r = append_packet(&packet, storage_enabled);
                    if (r != MC100_OK) break;
                }
                if (r != MC100_OK) break;
                portENTER_CRITICAL(&core_lock);
                r = mc100_audio_snapshot_release(audio, generation);
                portEXIT_CRITICAL(&core_lock);
                if (r != MC100_OK) break;
                snapshot_copied = true;
            } else if (available != MC100_NOT_READY) { r = available; break; }
        }
        bool empty = true;
        if (snapshot_copied) {
            portENTER_CRITICAL(&core_lock);
            mc100_result_t popped = mc100_audio_pop(audio, &packet);
            portEXIT_CRITICAL(&core_lock);
            if (popped == MC100_OK) {
                empty = false;
                r = append_packet(&packet, storage_enabled);
                if (r != MC100_OK) break;
            } else if (popped != MC100_NOT_READY) { r = popped; break; }
            r = storage_enabled ? mc100_writer_checkpoint(writer, mc100_platform_now_ms() - started) : MC100_OK;
            if (r != MC100_OK) break;
        }
        if (done && empty) break;
        if (mc100_platform_now_ms() >= deadline) { r = MC100_TIMEOUT; break; }
        if (empty) (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    }
    stop_intent();
    if (!join_audio()) {
        reset_required = true; quiescence_failed = true;
        /* No abandon/unmount/release/free while a worker may still own state. */
        printf("%s result=6 reset_required=1 stage=audio_not_quiescent\n", operation); return;
    }
    mc100_result_t capture_result;
    (void)worker_state(&capture_result);
    mc100_audio_status_t status = {0};
    mc100_audio_stats_t stats = {0};
    portENTER_CRITICAL(&core_lock);
    (void)mc100_audio_status(audio, generation, &status);
    (void)mc100_audio_stats(audio, &stats);
    portEXIT_CRITICAL(&core_lock);
    uint32_t reason = capture_result == MC100_FULL || status.error == MC100_FULL ? MC100_INCIDENT_QUEUE_OVERFLOW :
                      capture_result != MC100_OK || status.error != MC100_OK ? MC100_INCIDENT_MIC_IO : 0;
    if (r == MC100_OK && began && status.cutoff_valid && written_frames &&
        packet.frame.seq == status.last_accepted_seq) {
        if (!reason && (written_frames != frames || status.last_accepted_seq != final_seq)) {
            r = MC100_CORRUPT;
        } else if (storage_enabled) {
            r = mc100_writer_close_through(writer, generation, status.last_accepted_seq, reason);
        }
    } else if (r == MC100_OK) r = capture_result != MC100_OK ? capture_result : MC100_CORRUPT;
    mc100_writer_status_t ws = {0};
    if (storage_enabled) (void)mc100_writer_status(writer, &ws);
    if (r != MC100_OK) {
        reset_required = true;
        if (storage_enabled) abandon_storage(r == MC100_FULL ? MC100_INCIDENT_STORAGE_FULL : MC100_INCIDENT_STORAGE_IO);
    }
    if (reason) reset_required = true;
    /* The producer acknowledged quiescence; FIFO compaction needs no IRQ lock. */
    (void)mc100_audio_release(audio, generation);
    printf("%s result=%d gen=%" PRIu64 " frames=%" PRIu64 " samples=%" PRIu64
           " min=%d max=%d energy=%" PRIu64 " overflow=%" PRIu32 " queue_peak=%u drops=%" PRIu64
           " elapsed_ms=%" PRIu64 " heap=%u audio_stack_free=%u storage_stack_free=%u reason=%" PRIu32 " reset_required=%u",
           operation, r != MC100_OK ? r : capture_result, generation, written_frames, written_frames * MC100_FRAME_SAMPLES,
           written_frames ? sample_min : 0, written_frames ? sample_max : 0, energy, overflow_count,
           (unsigned)stats.peak, stats.drops, mc100_platform_now_ms() - started,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)uxTaskGetStackHighWaterMark(audio_task), (unsigned)uxTaskGetStackHighWaterMark(NULL),
           reason, (unsigned)reset_required);
    if (storage_enabled) printf(" path=%s_%" PRIu64 "_%" PRIu32 "%s\n",
                                boot_text, generation, ws.segment_index, reason ? ".partial.wav" : ".wav");
    else puts(" storage_enabled=0 saved=0");
}

static mc100_result_t list_file(void *context, const char *path)
{
    (void)context;
    if (!mc100_path_valid(path)) return MC100_OK;
    uint64_t size;
    mc100_result_t r = mc100_platform_storage_io()->stat(mc100_platform_storage_context(), path, &size);
    if (r == MC100_OK) printf("FILE %s %" PRIu64 "\n", path, size);
    return r;
}

static void read_file(const mc100_evt_command_t *command)
{
    uint8_t bytes[MC100_EVT_READ_BYTES];
    mc100_file_t file = NULL;
    const mc100_io_t *io = mc100_platform_storage_io();
    void *context = mc100_platform_storage_context();
    mc100_result_t r = io->open_read(context, command->path, &file);
    size_t actual = 0;
    if (r == MC100_OK) r = io->read_at(context, file, command->offset, bytes, command->count, &actual);
    if (file) { mc100_result_t closed = io->close(context, file); if (r == MC100_OK) r = closed; }
    if (r != MC100_OK) { printf("READ result=%d\n", r); return; }
    printf("BEGIN %s %" PRIu64 " %u\n", command->path, command->offset, (unsigned)actual);
    for (size_t i = 0; i < actual; ++i) {
        printf("%02x", bytes[i]);
        if (i % 64 == 63 || i + 1 == actual) putchar('\n');
    }
    printf("END %08" PRIx32 "\n", mc100_crc32(bytes, actual));
}

static void print_status(void)
{
    portENTER_CRITICAL(&core_lock);
    uint64_t sequence = next_seq;
    portEXIT_CRITICAL(&core_lock);
    int millivolts = 0;
    mc100_result_t adc = mc100_platform_adc_mv(&millivolts);
    printf("STATUS mode=EVT_USB_BENCH battery_protection=bypassed_no_battery vad=disabled mounted=%u reset_required=%u card=%u adc_result=%d adc_pin_mv=%d heap=%u min_heap=%u next_seq=%" PRIu64 "\n",
           (unsigned)mounted, (unsigned)reset_required, (unsigned)mc100_platform_card_present(), adc,
           millivolts, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), sequence);
}

void mc100_evt_run(void)
{
    storage_task = xTaskGetCurrentTaskHandle();
    puts("MC100 EVT_USB_BENCH USB-only/no-battery; manual record; VAD disabled; audio statistics do not prove acoustic validity");
    mc100_result_t board = mc100_platform_board_init();
    mc100_result_t mount = board == MC100_OK ? mc100_platform_storage_mount() : board;
    mounted = mount == MC100_OK;
    uint8_t boot[16];
    esp_fill_random(boot, sizeof(boot));
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(boot); ++i) { boot_text[i * 2] = hex[boot[i] >> 4]; boot_text[i * 2 + 1] = hex[boot[i] & 15]; }
    audio = mc100_audio_create();
    if (mounted) writer = mc100_writer_create(mc100_platform_storage_io(), mc100_platform_storage_context(), boot);
    /* Missing/unmountable media disables recording, not a standalone mic test. */
    if (board != MC100_OK || !audio || xTaskCreatePinnedToCore(audio_worker, "evt_audio", 8192, NULL, 20, &audio_task, 1) != pdPASS) reset_required = true;
    printf("INIT board=%d mount=%d reset_required=%u\n", board, mount, (unsigned)reset_required);
    print_status();
    puts("MC100_READY"); fflush(stdout);
    mc100_evt_line_t line = {0};
    for (;;) {
        int byte = fgetc(stdin);
        if (byte == EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        mc100_evt_command_t command;
        int complete = mc100_evt_line_feed(&line, (unsigned char)byte, &command);
        if (!complete) continue;
        if (complete < 0) puts("ERROR invalid_command");
        else if (command.kind == EVT_SYNC) printf("SYNC %" PRIu32 "\n", command.token);
        else if (command.kind == EVT_STATUS) print_status();
        else if (command.kind == EVT_RECORD) capture_seconds(command.seconds, true);
        else if (command.kind == EVT_CAPTURE) capture_seconds(command.seconds, false);
        else if (quiescence_failed) puts("ERROR reset_required_worker_not_quiescent");
        else if (!mounted) puts("ERROR storage_unavailable");
        else if (command.kind == EVT_LIST) {
            mc100_result_t r = mc100_platform_storage_io()->list(mc100_platform_storage_context(), list_file, NULL);
            printf("LIST result=%d\n", r);
        } else if (command.kind == EVT_READ) read_file(&command);
        puts("MC100_READY"); fflush(stdout);
    }
}
