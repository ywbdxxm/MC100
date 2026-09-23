#include "mc100_platform.h"
#include "mc100_supervisor.h"
#include "mc100_vad.h"
#include "mc100_upload.h"
#include "mc100_battery_policy.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    bool monitor_stop;
    mc100_battery_policy_t *battery;
    mc100_supervisor_t *supervisor;
    TaskHandle_t monitor_task;
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
    mc100_result_t result = mc100_platform_audio_start();
    if (result == MC100_OK) runtime->audio_ready = true;
    return result;
}

static mc100_result_t product_audio_stop(product_runtime_ctx_t *runtime)
{
    if (!runtime || !runtime->audio_ready) return MC100_OK;
    mc100_result_t result = mc100_platform_audio_stop();
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
    } else if (capture_active && !runtime->audio_ready) {
        /* This is primarily a recovery hook.  The normal boot path starts
         * audio in driver_ready(), after recovery and before READY/ARM. */
        (void)product_audio_start(runtime);
    }
}

static mc100_battery_result_t product_battery_sample(
    product_runtime_ctx_t *runtime, uint64_t at)
{
    int millivolts = 0;
    mc100_result_t result = mc100_platform_adc_mv(&millivolts);
    /* ADC_BAT is the R6=1 Mohm/R7=330 kohm divider tap, not VBAT. */
    int64_t battery_mv = ((int64_t)millivolts * 1330 + 165) / 330;
    return mc100_battery_step(runtime->battery, (uint32_t)battery_mv,
                              result == MC100_OK && millivolts > 0 &&
                                  battery_mv > 0,
                              at);
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
    bool previous_card = true;
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
        if (previous_card && !card)
            product_post_monitor_event(runtime, MC100_EV_CRITICAL, 0, at);
        previous_card = card;
        product_post_monitor_event(runtime, MC100_EV_TICK, 0, at);
        vTaskDelay(pdMS_TO_TICKS(1000));
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
    (void)context;
    return mc100_platform_audio_read(buffer, capacity, count, timeout_ms);
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
    if (runtime.board_ready)
        runtime.storage_ready = mc100_platform_storage_mount() == MC100_OK;

    deps.io = mc100_platform_storage_io();
    deps.io_ctx = mc100_platform_storage_context();
    deps.now_ms = product_now_ms;
    deps.clock_ctx = NULL;
    deps.battery_ready = product_battery_ready;
    deps.battery_ctx = &runtime;
    deps.driver_ready = product_driver_ready;
    deps.driver_ctx = &runtime;
    deps.pcm_read = product_pcm_read;
    deps.pcm_ctx = NULL;
    deps.vad = mc100_vad_fixed(&vad_state, 120U);
    deps.upload = mc100_upload_noop();
    deps.boot_id = boot_id;
    deps.event_lock = product_event_lock;
    deps.event_unlock = product_event_unlock;
    deps.event_lock_ctx = &runtime;

    if (!runtime.battery || !runtime.board_ready || !runtime.storage_ready ||
        deps.io == NULL) {
        (void)product_audio_stop(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
        vTaskDelete(NULL);
        return;
    }

    supervisor = mc100_supervisor_create(&deps);
    if (supervisor == NULL || mc100_supervisor_boot(supervisor) != MC100_OK) {
        mc100_supervisor_destroy(supervisor);
        (void)product_audio_stop(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
        vTaskDelete(NULL);
        return;
    }
    runtime.supervisor = supervisor;
    if (xTaskCreate(product_monitor_task, "mc100_monitor", 4096, &runtime,
                    6, &runtime.monitor_task) != pdPASS) {
        mc100_supervisor_destroy(supervisor);
        (void)product_audio_stop(&runtime);
        (void)product_storage_unmount(&runtime);
        mc100_battery_destroy(runtime.battery);
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
