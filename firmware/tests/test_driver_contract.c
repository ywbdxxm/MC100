#include <assert.h>
#include "mc100_driver_config.h"

int main(void)
{
    assert(MC100_DRIVER_I2S_PORT == 0);
    assert(MC100_DRIVER_DMA_DESCRIPTORS * MC100_DRIVER_DMA_BYTES == 3840);
    assert(MC100_DRIVER_SD_WIDTH == 1 && MC100_DRIVER_SD_KHZ == 20000);
    assert(MC100_GPIO_PDM_DATA == 1 && MC100_GPIO_PDM_CLK == 2);
    assert(MC100_GPIO_SD_D0 == 6 && MC100_GPIO_SD_CLK == 7 && MC100_GPIO_SD_CMD == 8);
    assert(MC100_GPIO_LED == 21 && MC100_GPIO_USB_DM == 19 && MC100_GPIO_USB_DP == 20);
    size_t remaining = MC100_DRIVER_STARTUP_BYTES;
    assert(mc100_startup_consume(&remaining, 319) == 319 && remaining == 961);
    assert(mc100_startup_consume(&remaining, 640) == 640 && remaining == 321);
    assert(mc100_startup_consume(&remaining, 640) == 321 && remaining == 0);
    assert(mc100_startup_consume(&remaining, 641) == 0);
    return 0;
}
