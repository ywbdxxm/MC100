/* The build-time selector keeps the verified EVT bench entry point available
 * while allowing the product supervisor/runtime to own the default image. */
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if defined(CONFIG_MC100_APP_PRODUCT)

/* Implemented by mc100_platform_espidf/product_runtime.c (Task 2.8). */
extern void mc100_product_run(void);

static void mc100_product_task(void *argument)
{
    (void)argument;
    mc100_product_run();
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
    if (xTaskCreatePinnedToCore(mc100_product_task, "mc100_product", 16384,
                                NULL, 8, NULL, 0) != pdPASS) {
        puts("MC100 product startup failed: no product task");
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
