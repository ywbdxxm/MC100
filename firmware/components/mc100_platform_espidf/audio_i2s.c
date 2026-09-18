#include "mc100_platform.h"
#include "mc100_driver_config.h"
#include "driver/i2s_pdm.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static i2s_chan_handle_t rx;
static TaskHandle_t owner;
static bool enabled;
static portMUX_TYPE overflow_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t overflows;

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t handle,
                                 i2s_event_data_t *event, void *context)
{
    (void)handle; (void)event; (void)context;
    portENTER_CRITICAL_ISR(&overflow_lock);
    if (overflows != UINT32_MAX) ++overflows;
    portEXIT_CRITICAL_ISR(&overflow_lock);
    return false;
}

uint32_t mc100_platform_audio_overflows(void)
{
    portENTER_CRITICAL(&overflow_lock);
    uint32_t count = overflows;
    portEXIT_CRITICAL(&overflow_lock);
    return count;
}

mc100_result_t mc100_platform_audio_stop(void)
{
    if (!rx) return MC100_OK;
    if (owner != xTaskGetCurrentTaskHandle()) return MC100_INVALID;
    if (enabled) {
        if (i2s_channel_disable(rx) != ESP_OK) return MC100_IO;
        enabled = false;
    }
    if (i2s_del_channel(rx) != ESP_OK) return MC100_IO;
    rx = NULL;
    owner = NULL;
    return MC100_OK;
}

mc100_result_t mc100_platform_audio_start(void)
{
    if (rx) return MC100_NOT_READY;
    owner = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&overflow_lock);
    overflows = 0;
    portEXIT_CRITICAL(&overflow_lock);
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(MC100_DRIVER_I2S_PORT, I2S_ROLE_MASTER);
    channel.dma_desc_num = MC100_DRIVER_DMA_DESCRIPTORS;
    channel.dma_frame_num = MC100_DRIVER_DMA_SAMPLES;
    esp_err_t err = i2s_new_channel(&channel, NULL, &rx);
    if (err != ESP_OK) { owner = NULL; return err == ESP_ERR_NO_MEM ? MC100_FULL : MC100_IO; }
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MC100_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {.clk = MC100_GPIO_PDM_CLK, .din = MC100_GPIO_PDM_DATA}
    };
#if CONFIG_MC100_PDM_RIGHT_SLOT
    pdm.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
#else
    pdm.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;
#endif
    err = i2s_channel_init_pdm_rx_mode(rx, &pdm);
    i2s_chan_info_t info;
    if (err == ESP_OK) err = i2s_channel_get_info(rx, &info);
    if (err == ESP_OK && info.total_dma_buf_size != MC100_DRIVER_DMA_DESCRIPTORS * MC100_DRIVER_DMA_BYTES)
        err = ESP_ERR_INVALID_SIZE;
    i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_overflow};
    if (err == ESP_OK) err = i2s_channel_register_event_callback(rx, &callbacks, NULL);
    if (err == ESP_OK) err = i2s_channel_enable(rx);
    if (err != ESP_OK) { (void)mc100_platform_audio_stop(); return MC100_IO; }
    enabled = true;
    uint8_t discard[MC100_DRIVER_DMA_BYTES];
    size_t remaining = MC100_DRIVER_STARTUP_BYTES;
    uint64_t deadline = mc100_platform_now_ms() + 1000;
    while (remaining && mc100_platform_now_ms() < deadline) {
        size_t actual = 0;
        size_t request = remaining < sizeof(discard) ? remaining : sizeof(discard);
        err = i2s_channel_read(rx, discard, request, &actual, 100);
        (void)mc100_startup_consume(&remaining, actual);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) break;
    }
    if (remaining || (err != ESP_OK && err != ESP_ERR_TIMEOUT) || mc100_platform_audio_overflows()) {
        (void)mc100_platform_audio_stop();
        return MC100_IO;
    }
    return MC100_OK;
}

mc100_result_t mc100_platform_audio_read(uint8_t *out, size_t capacity,
                                       size_t *actual, uint32_t timeout_ms)
{
    if (actual) *actual = 0;
    if (!out || !capacity || !actual || timeout_ms > 1000) return MC100_INVALID;
    if (!enabled || owner != xTaskGetCurrentTaskHandle()) return MC100_NOT_READY;
    if (mc100_platform_audio_overflows()) return MC100_IO;
    esp_err_t err = i2s_channel_read(rx, out, capacity, actual, timeout_ms);
    if (mc100_platform_audio_overflows()) return MC100_IO;
    if (err == ESP_ERR_TIMEOUT) return MC100_TIMEOUT;
    return err == ESP_OK ? MC100_OK : MC100_IO;
}
