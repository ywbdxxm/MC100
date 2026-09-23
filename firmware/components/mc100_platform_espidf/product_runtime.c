#include "mc100_platform.h"
#include "mc100_supervisor.h"
#include "mc100_vad.h"
#include "mc100_upload.h"
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
} product_runtime_ctx_t;

static bool product_battery_ready(void *context)
{
    (void)context;
    int millivolts = 0;
    /* An unavailable ADC is not a safe basis for booting the product loop. */
    return mc100_platform_adc_mv(&millivolts) == MC100_OK && millivolts > 0;
}

static bool product_driver_ready(void *context)
{
    const product_runtime_ctx_t *runtime = (const product_runtime_ctx_t *)context;
    return runtime != NULL && runtime->board_ready && runtime->storage_ready &&
           runtime->audio_ready;
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
    product_runtime_ctx_t runtime = {0};
    uint8_t boot_id[16];
    mc100_supervisor_deps_t deps = {0};
    mc100_supervisor_t *supervisor = NULL;
    mc100_vad_fixed_t vad_state = {0};

    esp_fill_random(boot_id, sizeof(boot_id));
    runtime.board_ready = mc100_platform_board_init() == MC100_OK;
    if (runtime.board_ready)
        runtime.storage_ready = mc100_platform_storage_mount() == MC100_OK;
    if (runtime.board_ready && runtime.storage_ready)
        runtime.audio_ready = mc100_platform_audio_start() == MC100_OK;

    deps.io = mc100_platform_storage_io();
    deps.io_ctx = mc100_platform_storage_context();
    deps.now_ms = product_now_ms;
    deps.clock_ctx = NULL;
    deps.battery_ready = product_battery_ready;
    deps.battery_ctx = NULL;
    deps.driver_ready = product_driver_ready;
    deps.driver_ctx = &runtime;
    deps.pcm_read = product_pcm_read;
    deps.pcm_ctx = NULL;
    deps.vad = mc100_vad_fixed(&vad_state, 120U);
    deps.upload = mc100_upload_noop();
    deps.boot_id = boot_id;

    if (!runtime.board_ready || !runtime.storage_ready || !runtime.audio_ready ||
        deps.io == NULL) {
        (void)mc100_platform_audio_stop();
        (void)mc100_platform_storage_unmount();
        vTaskDelete(NULL);
        return;
    }

    supervisor = mc100_supervisor_create(&deps);
    if (supervisor == NULL || mc100_supervisor_boot(supervisor) != MC100_OK) {
        mc100_supervisor_destroy(supervisor);
        (void)mc100_platform_audio_stop();
        (void)mc100_platform_storage_unmount();
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        (void)mc100_supervisor_tick(supervisor);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
