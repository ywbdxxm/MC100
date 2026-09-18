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
    if (xTaskCreatePinnedToCore(evt_task, "evt_storage", 12288, NULL, 8, NULL, 0) != pdPASS) {
        puts("MC100 EVT startup failed: no storage task; recording disabled");
        fflush(stdout);
    }
}
