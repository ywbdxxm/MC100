idf_component_register(
    SRCS "audio_i2s.c" "sd_fat.c" "board_io.c" "product_runtime.c"
    INCLUDE_DIRS "include"
    REQUIRES mc100_core mc100_storage mc100_audio mc100_supervisor
    PRIV_REQUIRES freertos esp_hw_support esp_system mc100_board esp_driver_i2s esp_driver_sdmmc esp_driver_gpio esp_adc esp_timer fatfs sdmmc
)
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
