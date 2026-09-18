idf_component_register(SRCS "app_main.c" "evt_capture.c" "evt_commands.c"
                       PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage mc100_audio mc100_platform_espidf esp_hw_support heap
                       INCLUDE_DIRS "")
