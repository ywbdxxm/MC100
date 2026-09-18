#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    puts("MC100 infrastructure only / recording not implemented");
    fflush(stdout);

    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}
