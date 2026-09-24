set(MC100_PLATFORM_SRCS "audio_i2s.c" "sd_fat.c" "board_io.c")
set(MC100_PLATFORM_REQUIRES mc100_core mc100_storage mc100_audio)
if(CONFIG_MC100_LEGACY_RUNTIME)
    list(APPEND MC100_PLATFORM_SRCS "product_runtime.c")
    list(APPEND MC100_PLATFORM_REQUIRES mc100_supervisor)
endif()

idf_component_register(
    SRCS ${MC100_PLATFORM_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${MC100_PLATFORM_REQUIRES}
    PRIV_REQUIRES freertos esp_hw_support esp_system mc100_board esp_driver_i2s esp_driver_sdmmc esp_driver_gpio esp_adc esp_timer fatfs sdmmc
)
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
