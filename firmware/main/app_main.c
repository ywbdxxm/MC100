/* The build-time selector keeps the verified EVT bench entry point available
 * while allowing the minimal recorder to own the default image. */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_MC100_APP_PRODUCT)

#include "record_loop.h"

static void mc100_record_task(void *argument)
{
    (void)argument;
    mc100_record_run();
    vTaskDelete(NULL);
}

#else

#include "evt_capture.h"

static void evt_task(void *argument)
{
    (void)argument;
    mc100_evt_run();
    vTaskDelete(NULL);
}

#endif

void app_main(void)
{
#if defined(CONFIG_MC100_APP_PRODUCT)
    if (xTaskCreatePinnedToCore(mc100_record_task, "mc100_recorder",
                                MC100_RECORD_STORAGE_STACK_BYTES,
                                NULL, 8, NULL, 0) != pdPASS) {
        puts("MC100 recorder startup failed: no storage task");
        fflush(stdout);
    }
#else
    /* exFAT LFN + reserve validation reached 9,768 bytes on the USB bench.
     * Keep >=25% stack margin without reducing the audio buffers. */
    if (xTaskCreatePinnedToCore(evt_task, "evt_storage", 16384, NULL, 8, NULL,
                                0) != pdPASS) {
        puts("MC100 EVT startup failed: no storage task; recording disabled");
        fflush(stdout);
    }
#endif
}
