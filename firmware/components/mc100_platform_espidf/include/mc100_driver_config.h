#ifndef MC100_DRIVER_CONFIG_H
#define MC100_DRIVER_CONFIG_H
#include <stddef.h>
#include "board_mc100_v1.h"
enum {
    MC100_DRIVER_I2S_PORT = 0,
    MC100_DRIVER_DMA_DESCRIPTORS = 6,
    MC100_DRIVER_DMA_SAMPLES = 320,
    MC100_DRIVER_DMA_BYTES = 640,
    MC100_DRIVER_STARTUP_BYTES = 1280,
    MC100_DRIVER_SD_WIDTH = 1,
    MC100_DRIVER_SD_KHZ = 20000,
    MC100_DRIVER_FILE_HANDLES = 8
};
static inline size_t mc100_startup_consume(size_t *remaining, size_t received)
{
    size_t drop = received < *remaining ? received : *remaining;
    *remaining -= drop;
    return drop;
}
#endif
