#include <assert.h>
#include <stdint.h>

#include "board_mc100_v1.h"
#include "mc100_types.h"

int main(void)
{
    assert(MC100_GPIO_PDM_DATA == 1);
    assert(MC100_GPIO_PDM_CLK == 2);
    assert(MC100_GPIO_BAT_ADC == 4);
    assert(MC100_GPIO_SD_CD == 5);
    assert(MC100_GPIO_SD_D0 == 6);
    assert(MC100_GPIO_SD_CLK == 7);
    assert(MC100_GPIO_SD_CMD == 8);
    assert(MC100_GPIO_LED == 21);
    assert(MC100_GPIO_LED != MC100_GPIO_SD_CLK);
    assert(MC100_GPIO_USB_DM == 19);
    assert(MC100_GPIO_USB_DP == 20);

    assert(MC100_HAS_PSRAM == 0);
    assert(MC100_HAS_SOFT_POWER_OFF == 0);
    assert(MC100_HAS_SD_POWER_SWITCH == 0);
    assert(MC100_HAS_CHARGER_STATUS == 0);

    assert(MC100_SAMPLE_RATE == 16000);
    assert(MC100_FRAME_SAMPLES == 320);
    assert(MC100_PREROLL_FRAMES == 100);
    assert(MC100_STREAM_FRAMES == 96);
    assert(MC100_SEGMENT_FRAMES == 15000);
    assert(MC100_FRAME_SAMPLES * sizeof(int16_t) == 640);
    assert(sizeof(mc100_generation_t) == 8);
    assert(sizeof(mc100_frame_t) == 648);
    assert(sizeof(mc100_packet_t) == 656);
    assert(sizeof(mc100_snapshot_t) == 24);
    assert(MC100_OK == 0 && MC100_TIMEOUT == 6);
    return 0;
}
