#include <stdio.h>
#include "evt_capture.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void evt_task(void *argument)
{
    (void)argument;
    mc100_evt_run();
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* exFAT LFN + reserve validation reached 9,768 bytes on the USB bench.
     * Keep >=25% stack margin without reducing the audio buffers. */
    if (xTaskCreatePinnedToCore(evt_task, "evt_storage", 16384, NULL, 8, NULL, 0) != pdPASS) {
        puts("MC100 EVT startup failed: no storage task; recording disabled");
        fflush(stdout);
    }
}
