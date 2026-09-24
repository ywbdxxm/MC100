set(MC100_MAIN_SRCS "app_main.c")
if(CONFIG_MC100_APP_PRODUCT)
    list(APPEND MC100_MAIN_SRCS "record_loop.c")
else()
    list(APPEND MC100_MAIN_SRCS "evt_capture.c" "evt_commands.c")
endif()

# IDF expands component requirements before loading CONFIG. Keep the portable
# recorder visible to main in every profile; EVT does not reference its code,
# so it is omitted by the final link when the product source is not selected.
idf_component_register(
    SRCS ${MC100_MAIN_SRCS}
    PRIV_REQUIRES freertos mc100_board mc100_core mc100_storage
                  mc100_audio mc100_platform_espidf mc100_recorder
                  esp_hw_support heap
    INCLUDE_DIRS "")
