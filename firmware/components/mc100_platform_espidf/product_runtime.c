#include "mc100_platform.h"
#include "mc100_supervisor.h"
#include "mc100_vad.h"
#include "mc100_upload.h"
#include "mc100_battery_policy.h"
#include "mc100_product_audio_protocol.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The I2S task is the only caller of mc100_platform_audio_*().  A fixed
 * internal-RAM queue decouples the 20 ms PDM cadence from bounded SD writes.
 * 96 items is the same frame budget as the portable audio stream (1.92 s of
 * PCM); a full queue is a safety fault, never an unbounded allocation. */
enum {
    MC100_PRODUCT_PCM_QUEUE_FRAMES = MC100_STREAM_FRAMES,
    MC100_PRODUCT_PCM_BYTES = MC100_FRAME_SAMPLES * sizeof(int16_t),
    MC100_PRODUCT_COMMAND_QUEUE_CAPACITY = 4
};

typedef enum {
    PRODUCT_AUDIO_CMD_START = 1,
    PRODUCT_AUDIO_CMD_STOP,
    PRODUCT_AUDIO_CMD_SHUTDOWN
} product_audio_command_t;

typedef struct {
    product_audio_command_t id;
    uint32_t sequence;
} product_audio_command_item_t;

typedef struct {
    size_t count;
    uint8_t bytes[MC100_PRODUCT_PCM_BYTES];
} product_pcm_item_t;

/* One product instance exists.  Static storage makes the no-PSRAM placement
 * explicit and keeps the queue out of the product task's stack. */
static DRAM_ATTR StaticQueue_t product_pcm_queue_struct;
static DRAM_ATTR uint8_t product_pcm_queue_storage[
    MC100_PRODUCT_PCM_QUEUE_FRAMES * sizeof(product_pcm_item_t)];
static DRAM_ATTR StaticQueue_t product_command_queue_struct;
static DRAM_ATTR uint8_t product_command_queue_storage[
    MC100_PRODUCT_COMMAND_QUEUE_CAPACITY *
    sizeof(product_audio_command_item_t)];

/*
 * Product-only target adapter.  The supervisor remains the owner of the
 * portable state, audio and writer objects; this file only owns board bring-up
 * and the FreeRTOS call loop.  The EVT USB bench path is intentionally not
 * reused here because it has a different lifetime and ownership model.
 */
typedef struct {
    bool board_ready;
    bool storage_ready;
    bool audio_ready;
    bool storage_quiesced;
    volatile bool monitor_stop;
    mc100_battery_policy_t *battery;
    mc100_supervisor_t *supervisor;
    TaskHandle_t monitor_task;
    TaskHandle_t audio_task;
    QueueHandle_t pcm_queue;
    QueueHandle_t command_queue;
    TaskHandle_t audio_ack_task;
    mc100_result_t audio_command_result;
    uint32_t audio_ack_sequence;
    mc100_result_t audio_worker_error;
    bool audio_worker_running;
    mc100_product_audio_protocol_t audio_protocol;
    portMUX_TYPE event_mux;
} product_runtime_ctx_t;

static const mc100_battery_config_t product_battery_config = {
    .low_mv = 3600, .critical_mv = 3450, .resume_mv = 3800,
    .low_duration_ms = 3000, .resume_duration_ms = 30000
};

static void product_event_lock(void *context)
{
    product_runtime_ctx_t *runtime = context;
    portENTER_CRITICAL(&runtime->event_mux);
}

static void product_event_unlock(void *context)
{
    product_runtime_ctx_t *runtime = context;
    portEXIT_CRITICAL(&runtime->event_mux);
}

static void product_audio_ack(product_runtime_ctx_t *runtime,
                              uint32_t sequence, mc100_result_t result)
{
    TaskHandle_t waiter;
    portENTER_CRITICAL(&runtime->event_mux);
    runtime->audio_command_result = result;
    runtime->audio_ack_sequence = sequence;
    waiter = runtime->audio_ack_task;
    if (waiter != NULL) (void)xTaskNotifyGive(waiter);
    portEXIT_CRITICAL(&runtime->event_mux);
}

static void product_audio_worker(void *argument)
{
    product_runtime_ctx_t *runtime = argument;
    product_audio_command_item_t command;
    for (;;) {
        if (xQueueReceive(runtime->command_queue, &command, portMAX_DELAY) !=
            pdPASS)
            continue;
        if (command.id == PRODUCT_AUDIO_CMD_SHUTDOWN) {
            product_audio_ack(runtime, command.sequence, MC100_OK);
            vTaskDelete(NULL);
            return;
        }
        if (command.id == PRODUCT_AUDIO_CMD_STOP) {
            /* A read error may have stopped the worker before the supervisor
             * issued its cleanup command.  STOP is still an acknowledged
             * idempotent quiescence operation; the latched read error remains
             * visible through product_pcm_read(). */
            product_audio_ack(runtime, command.sequence, MC100_OK);
            continue;
        }
        if (command.id != PRODUCT_AUDIO_CMD_START) continue;

        /* START is idempotent so ARM retries cannot create a second I2S
         * owner.  The queue is empty before every new capture epoch. */
        portENTER_CRITICAL(&runtime->event_mux);
        bool already_running = runtime->audio_worker_running;
        runtime->audio_worker_error = MC100_OK;
        portEXIT_CRITICAL(&runtime->event_mux);
        if (already_running) {
            product_audio_ack(runtime, command.sequence, MC100_OK);
            continue;
        }
        (void)xQueueReset(runtime->pcm_queue);
        mc100_result_t result = mc100_platform_audio_start();
        if (result != MC100_OK) {
            product_audio_ack(runtime, command.sequence, result);
            continue;
        }
        portENTER_CRITICAL(&runtime->event_mux);
        runtime->audio_worker_running = true;
        portEXIT_CRITICAL(&runtime->event_mux);
        product_audio_ack(runtime, command.sequence, MC100_OK);

        bool running = true;
        while (running) {
            product_audio_command_item_t pending;
            while (xQueueReceive(runtime->command_queue, &pending, 0) ==
                   pdPASS) {
                if (pending.id == PRODUCT_AUDIO_CMD_STOP ||
                    pending.id == PRODUCT_AUDIO_CMD_SHUTDOWN) {
                    running = false;
                    if (pending.id == PRODUCT_AUDIO_CMD_SHUTDOWN)
                        command = pending;
                    break;
                }
            }
            if (!running) break;

            product_pcm_item_t item = {0};
            mc100_result_t read_result = mc100_platform_audio_read(
                item.bytes, sizeof(item.bytes), &item.count, 20);
            if (read_result == MC100_TIMEOUT) continue;
            if (read_result != MC100_OK) {
                portENTER_CRITICAL(&runtime->event_mux);
                runtime->audio_worker_error = read_result;
                portEXIT_CRITICAL(&runtime->event_mux);
                /* Stop the owner cleanly; the consumer will route the
                 * latched error to FAULT instead of accepting a gap. */
                break;
            }
            if (item.count == 0) continue;
            if (xQueueSend(runtime->pcm_queue, &item, 0) != pdPASS) {
                portENTER_CRITICAL(&runtime->event_mux);
                runtime->audio_worker_error = MC100_FULL;
                portEXIT_CRITICAL(&runtime->event_mux);
                /* Keep reading until STOP so a full software queue does not
                 * leave the hardware DMA owner blocked.  New data is safely
                 * discarded and the supervisor fails closed. */
            }
        }

        mc100_result_t stop_result = mc100_platform_audio_stop();
        portENTER_CRITICAL(&runtime->event_mux);
        runtime->audio_worker_running = false;
        mc100_result_t worker_error = runtime->audio_worker_error;
        portEXIT_CRITICAL(&runtime->event_mux);
        (void)xQueueReset(runtime->pcm_queue);
        if (stop_result == MC100_OK && worker_error != MC100_OK)
            stop_result = worker_error;
        product_audio_ack(runtime, command.sequence, stop_result);
        if (command.id == PRODUCT_AUDIO_CMD_SHUTDOWN) {
            vTaskDelete(NULL);
            return;
        }
        command.id = 0;
        command.sequence = 0;
    }
}

static mc100_result_t product_audio_command(product_runtime_ctx_t *runtime,
                                            product_audio_command_t command)
{
    if (!runtime || !runtime->audio_task || !runtime->command_queue)
        return MC100_NOT_READY;
    TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    uint32_t sequence = mc100_product_audio_begin(&runtime->audio_protocol);
    portENTER_CRITICAL(&runtime->event_mux);
    runtime->audio_ack_task = waiter;
    runtime->audio_command_result = MC100_TIMEOUT;
    runtime->audio_ack_sequence = 0;
    portEXIT_CRITICAL(&runtime->event_mux);
    (void)ulTaskNotifyTake(pdTRUE, 0);
    product_audio_command_item_t item = {.id = command, .sequence = sequence};
    if (xQueueSend(runtime->command_queue, &item, pdMS_TO_TICKS(100)) !=
        pdPASS) {
        portENTER_CRITICAL(&runtime->event_mux);
        if (runtime->audio_ack_task == waiter) runtime->audio_ack_task = NULL;
        portEXIT_CRITICAL(&runtime->event_mux);
        return MC100_FULL;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        TickType_t remaining = now < deadline ? deadline - now : 0;
        if (ulTaskNotifyTake(pdTRUE, remaining) != 1) {
            portENTER_CRITICAL(&runtime->event_mux);
            if (runtime->audio_ack_task == waiter) runtime->audio_ack_task = NULL;
            portEXIT_CRITICAL(&runtime->event_mux);
            return MC100_TIMEOUT;
        }
        portENTER_CRITICAL(&runtime->event_mux);
        runtime->audio_protocol.acknowledged_sequence =
            runtime->audio_ack_sequence;
        bool match = mc100_product_audio_ack_matches(
            &runtime->audio_protocol, sequence);
        mc100_result_t result = runtime->audio_command_result;
        if (match && runtime->audio_ack_task == waiter)
            runtime->audio_ack_task = NULL;
        portEXIT_CRITICAL(&runtime->event_mux);
        if (match) return result;
    }
}

static mc100_result_t product_audio_control(void *context,
                                            mc100_supervisor_audio_control_t command,
                                            mc100_generation_t generation)
{
    (void)generation;
    product_runtime_ctx_t *runtime = context;
    return product_audio_command(runtime,
        command == MC100_SUPERVISOR_AUDIO_START ? PRODUCT_AUDIO_CMD_START :
        PRODUCT_AUDIO_CMD_STOP);
}

static bool product_audio_worker_init(product_runtime_ctx_t *runtime)
{
    runtime->pcm_queue = xQueueCreateStatic(MC100_PRODUCT_PCM_QUEUE_FRAMES,
        sizeof(product_pcm_item_t), product_pcm_queue_storage,
        &product_pcm_queue_struct);
    runtime->command_queue = xQueueCreateStatic(
        MC100_PRODUCT_COMMAND_QUEUE_CAPACITY,
        sizeof(product_audio_command_item_t),
        product_command_queue_storage, &product_command_queue_struct);
    if (!runtime->pcm_queue || !runtime->command_queue) return false;
    if (xTaskCreatePinnedToCore(product_audio_worker, "mc100_audio",
                                8192, runtime, 20, &runtime->audio_task, 1) !=
        pdPASS) {
        runtime->audio_task = NULL;
        return false;
    }
    return true;
}

static mc100_result_t product_audio_worker_shutdown(product_runtime_ctx_t *runtime)
{
    if (!runtime || !runtime->audio_task) return MC100_OK;
    mc100_result_t result = product_audio_command(
        runtime, PRODUCT_AUDIO_CMD_SHUTDOWN);
    if (result == MC100_OK) runtime->audio_task = NULL;
    return result;
}

static void product_audio_quarantine(product_runtime_ctx_t *runtime)
{
    (void)runtime;
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

/*
 * I2S and FatFs are owner-task resources.  Keep their lifecycle in small,
 * typed helpers so a future LOW/CRITICAL event path can stop and restart the
 * hardware without letting the monitor task call a non-owner API.  The
 * Monitor code only posts bounded intents; these helpers are called only from
 * this product task (boot, owner cleanup, and recovery).
 */
static mc100_result_t product_audio_start(product_runtime_ctx_t *runtime)
{
    if (!runtime || !runtime->board_ready || !runtime->storage_ready)
        return MC100_NOT_READY;
    if (runtime->audio_ready) return MC100_OK;
    mc100_result_t result = product_audio_command(
        runtime, PRODUCT_AUDIO_CMD_START);
    if (result == MC100_OK) runtime->audio_ready = true;
    return result;
}

static mc100_result_t product_audio_stop(product_runtime_ctx_t *runtime)
{
    if (!runtime || !runtime->audio_ready) return MC100_OK;
    mc100_result_t result = product_audio_command(
        runtime, PRODUCT_AUDIO_CMD_STOP);
    if (result == MC100_OK) runtime->audio_ready = false;
    return result;
}

static mc100_result_t product_storage_unmount(product_runtime_ctx_t *runtime)
{
    if (!runtime || !runtime->storage_ready) return MC100_OK;
    mc100_result_t result = mc100_platform_storage_unmount();
    if (result == MC100_OK) runtime->storage_ready = false;
    return result;
}

static void product_audio_reconcile(product_runtime_ctx_t *runtime,
                                     const mc100_supervisor_t *supervisor)
{
    if (!runtime || !supervisor) return;
    mc100_state_id_t state = mc100_supervisor_state(supervisor);
    bool capture_active = state == MC100_LISTEN || state == MC100_RECORD;

    /* LOW/FAULT/HOLD actions have already quiesced the portable audio owner
     * by the time tick returns. Stop physical DMA from this same task; a
     * monitor task must never call the owner-only platform API. */
    if (!capture_active &&
        (state == MC100_LOW_BAT || state == MC100_LOW_BAT_HOLD ||
         state == MC100_FAULT)) {
        (void)product_audio_stop(runtime);
    }
}

static mc100_battery_result_t product_battery_sample(
    product_runtime_ctx_t *runtime, uint64_t at)
{
    int millivolts = 0;
    mc100_result_t result = mc100_platform_adc_mv(&millivolts);
    /* ADC_BAT is the R6=1 Mohm/R7=330 kohm divider tap, not VBAT. */
    bool valid = result == MC100_OK && millivolts > 0;
    int64_t battery_mv = valid ?
        ((int64_t)millivolts * 1330 + 165) / 330 : 0;
    if (battery_mv <= 0 || battery_mv > UINT32_MAX) valid = false;
    return mc100_battery_step(runtime->battery,
                              valid ? (uint32_t)battery_mv : 0, valid, at);
}

static void product_post_monitor_event(product_runtime_ctx_t *runtime,
                                       mc100_event_id_t id, uint32_t detail,
                                       uint64_t at)
{
    mc100_event_t event = {.id = id, .detail = detail, .now_ms = at,
                           .global = true};
    (void)mc100_supervisor_post_event(runtime->supervisor, &event);
}

static void product_monitor_task(void *argument)
{
    product_runtime_ctx_t *runtime = argument;
    bool stable_card = mc100_platform_card_present();
    unsigned card_mismatch = 0;
    while (!runtime->monitor_stop) {
        uint64_t at = mc100_platform_now_ms();
        switch (product_battery_sample(runtime, at)) {
        case MC100_BATTERY_LOW:
            product_post_monitor_event(runtime, MC100_EV_LOW, 0, at);
            break;
        case MC100_BATTERY_CRITICAL:
            product_post_monitor_event(runtime, MC100_EV_CRITICAL, 0, at);
            break;
        case MC100_BATTERY_INVALID:
            product_post_monitor_event(runtime, MC100_EV_FAULT,
                                       MC100_FAULT_ADC_INVALID, at);
            break;
        case MC100_BATTERY_RECOVERED:
            product_post_monitor_event(runtime, MC100_EV_RECOVERED_POWER, 0,
                                       at);
            break;
        case MC100_BATTERY_NORMAL:
        default:
            break;
        }
        bool card = mc100_platform_card_present();
        if (card != stable_card) {
            if (++card_mismatch >= 2) {
                stable_card = card;
                card_mismatch = 0;
                if (!stable_card)
                    product_post_monitor_event(runtime, MC100_EV_FAULT,
                                               MC100_FAULT_STORAGE_IO, at);
            }
        } else {
            card_mismatch = 0;
        }
        product_post_monitor_event(runtime, MC100_EV_TICK, 0, at);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

static void product_reconcile_storage(product_runtime_ctx_t *runtime,
                                      mc100_supervisor_t *supervisor)
{
    mc100_state_id_t state = mc100_supervisor_state(supervisor);
    if ((state == MC100_LOW_BAT_HOLD || state == MC100_FAULT) &&
        !runtime->storage_quiesced) {
        mc100_writer_status_t status = {0};
        if (mc100_supervisor_writer_status(supervisor, &status) == MC100_OK &&
            !status.active && product_audio_stop(runtime) == MC100_OK &&
            product_storage_unmount(runtime) == MC100_OK)
            runtime->storage_quiesced = true;
    }
    if (state == MC100_BOOT && runtime->storage_quiesced) {
        if (mc100_platform_storage_mount() == MC100_OK) {
            runtime->storage_ready = true;
            if (mc100_supervisor_rearm(supervisor) == MC100_OK)
                runtime->storage_quiesced = false;
            else
                (void)product_storage_unmount(runtime);
        }
    }
}

static bool product_battery_ready(void *context)
{
    product_runtime_ctx_t *runtime = context;
    if (!runtime || !runtime->board_ready || !runtime->storage_ready ||
        !runtime->battery)
        return false;
    mc100_battery_result_t result = product_battery_sample(
        runtime, mc100_platform_now_ms());
    return result == MC100_BATTERY_NORMAL &&
           mc100_battery_is_stable(runtime->battery);
}

static bool product_driver_ready(void *context)
{
    product_runtime_ctx_t *runtime = context;
    if (!runtime || !runtime->board_ready || !runtime->storage_ready)
        return false;

    /* supervisor_boot() performs recovery and writer preparation before this
     * callback.  Starting I2S lazily here guarantees no audio DMA is active
     * while recovery scans or repairs the card. */
    return product_audio_start(runtime) == MC100_OK;
}

static uint64_t product_now_ms(void *context)
{
    (void)context;
    return mc100_platform_now_ms();
}

static mc100_result_t product_pcm_read(void *context, uint8_t *buffer,
                                       size_t capacity, size_t *count,
                                       uint32_t timeout_ms)
{
    product_runtime_ctx_t *runtime = context;
    if (count) *count = 0;
    if (!runtime || !buffer || !count || capacity < MC100_PRODUCT_PCM_BYTES ||
        !runtime->pcm_queue)
        return MC100_INVALID;
    portENTER_CRITICAL(&runtime->event_mux);
    mc100_result_t worker_error = runtime->audio_worker_error;
    portEXIT_CRITICAL(&runtime->event_mux);
    if (worker_error != MC100_OK) return worker_error;
    product_pcm_item_t item = {0};
    TickType_t wait = pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(runtime->pcm_queue, &item, wait) != pdPASS)
        return MC100_OK; /* no frame yet is not a microphone fault */
    if (item.count > capacity || item.count > sizeof(item.bytes))
        return MC100_INVALID;
    memcpy(buffer, item.bytes, item.count);
    *count = item.count;
    return MC100_OK;
}

/* Product boot is deliberately fail-closed.  Do not mount/recover/preallocate
 * while the ADC has not produced a stable normal VBAT sample.  The loop is
 * bounded per iteration and yields, so a USB-only EVT setup cannot turn a
 * missing battery into repeated writes or a watchdog starvation. */
static bool product_wait_for_battery(product_runtime_ctx_t *runtime)
{
    uint64_t last_report = 0;
    for (;;) {
        uint64_t at = mc100_platform_now_ms();
        mc100_battery_result_t result = product_battery_sample(runtime, at);
        if (result == MC100_BATTERY_NORMAL &&
            mc100_battery_is_stable(runtime->battery))
            return true;
        if (at - last_report >= 5000) {
            printf("MC100_PRODUCT waiting_battery result=%d stable=%u\n",
                   (int)result,
                   (unsigned)mc100_battery_is_stable(runtime->battery));
            fflush(stdout);
            last_report = at;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool product_mount_until_ready(product_runtime_ctx_t *runtime)
{
    uint64_t last_report = 0;
    for (;;) {
        if (mc100_platform_storage_mount() == MC100_OK) {
            runtime->storage_ready = true;
            return true;
        }
        uint64_t at = mc100_platform_now_ms();
        if (at - last_report >= 5000) {
            printf("MC100_PRODUCT waiting_storage card=%u\n",
                   (unsigned)mc100_platform_card_present());
            fflush(stdout);
            last_report = at;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mc100_product_run(void)
{
    product_runtime_ctx_t runtime = {
        .event_mux = portMUX_INITIALIZER_UNLOCKED
    };
    uint8_t boot_id[16];
    mc100_supervisor_deps_t deps = {0};
    mc100_supervisor_t *supervisor = NULL;
    mc100_vad_fixed_t vad_state = {0};
    runtime.battery = mc100_battery_create(&product_battery_config);

    esp_fill_random(boot_id, sizeof(boot_id));
    runtime.board_ready = mc100_platform_board_init() == MC100_OK;
    if (!runtime.battery || !runtime.board_ready) {
        mc100_battery_destroy(runtime.battery);
        vTaskDelete(NULL);
        return;
    }
    (void)product_wait_for_battery(&runtime);
    (void)product_mount_until_ready(&runtime);

    deps.io = mc100_platform_storage_io();
    deps.io_ctx = mc100_platform_storage_context();
    deps.now_ms = product_now_ms;
    deps.clock_ctx = NULL;
    deps.battery_ready = product_battery_ready;
    deps.battery_ctx = &runtime;
    deps.driver_ready = product_driver_ready;
    deps.driver_ctx = &runtime;
    deps.pcm_read = product_pcm_read;
    deps.pcm_ctx = &runtime;
    deps.vad = mc100_vad_fixed(&vad_state, 120U);
    deps.upload = mc100_upload_noop();
    deps.boot_id = boot_id;
    deps.event_lock = product_event_lock;
    deps.event_unlock = product_event_unlock;
    deps.event_lock_ctx = &runtime;
    deps.audio_control = product_audio_control;
    deps.audio_control_ctx = &runtime;

    if (!runtime.battery || !runtime.board_ready || !runtime.storage_ready ||
        !product_audio_worker_init(&runtime) ||
        deps.io == NULL) {
        (void)product_audio_stop(&runtime);
        mc100_result_t shutdown_result = product_audio_worker_shutdown(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
        if (mc100_product_audio_shutdown_requires_quarantine(
                shutdown_result == MC100_OK))
            product_audio_quarantine(&runtime);
        vTaskDelete(NULL);
        return;
    }

    supervisor = mc100_supervisor_create(&deps);
    if (supervisor == NULL || mc100_supervisor_boot(supervisor) != MC100_OK) {
        mc100_supervisor_destroy(supervisor);
        (void)product_audio_stop(&runtime);
        mc100_result_t shutdown_result = product_audio_worker_shutdown(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
        if (mc100_product_audio_shutdown_requires_quarantine(
                shutdown_result == MC100_OK))
            product_audio_quarantine(&runtime);
        vTaskDelete(NULL);
        return;
    }
    runtime.supervisor = supervisor;
    if (xTaskCreate(product_monitor_task, "mc100_monitor", 4096, &runtime,
                    6, &runtime.monitor_task) != pdPASS) {
        mc100_supervisor_destroy(supervisor);
        (void)product_audio_stop(&runtime);
        mc100_result_t shutdown_result = product_audio_worker_shutdown(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
        if (mc100_product_audio_shutdown_requires_quarantine(
                shutdown_result == MC100_OK))
            product_audio_quarantine(&runtime);
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        (void)mc100_supervisor_tick(supervisor);
        product_audio_reconcile(&runtime, supervisor);
        product_reconcile_storage(&runtime, supervisor);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
