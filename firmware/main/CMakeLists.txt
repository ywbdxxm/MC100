set(MC100_MAIN_SRCS "app_main.c")
if(CONFIG_MC100_APP_PRODUCT)
    list(APPEND MC100_MAIN_SRCS "record_loop.c")
    idf_component_register(
        SRCS ${MC100_MAIN_SRCS}
        PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage
                      mc100_audio mc100_platform_espidf mc100_recorder
                      esp_hw_support heap
        INCLUDE_DIRS "")
else()
    list(APPEND MC100_MAIN_SRCS "evt_capture.c" "evt_commands.c")
    idf_component_register(
        SRCS ${MC100_MAIN_SRCS}
        PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage
                      mc100_audio mc100_platform_espidf esp_hw_support heap
        INCLUDE_DIRS "")
endif()
