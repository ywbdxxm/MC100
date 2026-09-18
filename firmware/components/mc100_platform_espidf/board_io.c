#include "mc100_platform.h"
#include "board_mc100_v1.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static bool board_ready;

mc100_result_t mc100_platform_board_init(void)
{
    if (board_ready) return MC100_OK;
    gpio_config_t led = {.pin_bit_mask = 1ULL << MC100_GPIO_LED,
                         .mode = GPIO_MODE_OUTPUT, .intr_type = GPIO_INTR_DISABLE};
    gpio_config_t cd = {.pin_bit_mask = 1ULL << MC100_GPIO_SD_CD,
                        .mode = GPIO_MODE_INPUT, .intr_type = GPIO_INTR_DISABLE};
    /* The board provides the CD pull-up. USB pins are never reconfigured. */
    if (gpio_set_level(MC100_GPIO_LED, 0) != ESP_OK ||
        gpio_config(&led) != ESP_OK || gpio_config(&cd) != ESP_OK) return MC100_IO;
    board_ready = true;
    return MC100_OK;
}

void mc100_platform_led(bool on) { if (board_ready) (void)gpio_set_level(MC100_GPIO_LED, on); }
bool mc100_platform_card_level(void) { return gpio_get_level(MC100_GPIO_SD_CD) != 0; }
bool mc100_platform_card_present(void)
{
#if CONFIG_MC100_SD_CD_ACTIVE_HIGH
    return mc100_platform_card_level();
#else
    return !mc100_platform_card_level();
#endif
}
uint64_t mc100_platform_now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

mc100_result_t mc100_platform_adc_mv(int *pin_mv)
{
    if (!pin_mv) return MC100_INVALID;
    *pin_mv = 0;
    if (!adc) {
        adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
        if (adc_oneshot_new_unit(&unit, &adc) != ESP_OK) return MC100_IO;
        adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        if (adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &channel) != ESP_OK) {
            (void)adc_oneshot_del_unit(adc); adc = NULL; return MC100_IO;
        }
    }
    if (!calibration) {
        adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3,
            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT
        };
        if (adc_cali_create_scheme_curve_fitting(&config, &calibration) != ESP_OK)
            return MC100_NOT_READY; /* Never replace unavailable calibration with guessed mV. */
    }
    int raw;
    if (adc_oneshot_read(adc, ADC_CHANNEL_3, &raw) != ESP_OK) return MC100_IO;
    return adc_cali_raw_to_voltage(calibration, raw, pin_mv) == ESP_OK ? MC100_OK : MC100_IO;
}
