#ifndef MC100_PLATFORM_H
#define MC100_PLATFORM_H
#include "mc100_io.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Audio APIs have one task owner. read preserves partial bytes even on timeout;
 * IO indicates a latched capture gap and must not be reported as clean audio. */
mc100_result_t mc100_platform_audio_start(void);
mc100_result_t mc100_platform_audio_read(uint8_t *, size_t, size_t *, uint32_t);
mc100_result_t mc100_platform_audio_stop(void);
uint32_t mc100_platform_audio_overflows(void);
/* Storage APIs and every io callback share one task owner. Unsynced close
 * abandons the mount without any new write; close all handles then unmount.
 * No automatic formatting, unlink, or cross-task cancellation is supported. */
mc100_result_t mc100_platform_storage_mount(void);
mc100_result_t mc100_platform_storage_unmount(void);
const mc100_io_t *mc100_platform_storage_io(void);
void *mc100_platform_storage_context(void);
mc100_result_t mc100_platform_board_init(void);
void mc100_platform_led(bool on);
bool mc100_platform_card_level(void);
bool mc100_platform_card_present(void);
mc100_result_t mc100_platform_adc_mv(int *pin_mv);
uint64_t mc100_platform_now_ms(void);
#ifdef __cplusplus
}
#endif
#endif
