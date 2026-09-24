set(MC100_PLATFORM_SRCS "audio_i2s.c" "sd_fat.c" "board_io.c")
if(CONFIG_MC100_LEGACY_RUNTIME)
    list(APPEND MC100_PLATFORM_SRCS "product_runtime.c")
    # IDF forbids CONFIG-dependent REQUIRES. Build the legacy Supervisor
    # implementation in this component only for the opt-in legacy profile;
    # the default product therefore has no mc100_supervisor graph edge.
    list(APPEND MC100_PLATFORM_SRCS
         "${CMAKE_CURRENT_LIST_DIR}/../mc100_supervisor/supervisor.c")
endif()

idf_component_register(
    SRCS ${MC100_PLATFORM_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES mc100_core mc100_storage mc100_audio
    PRIV_REQUIRES freertos esp_hw_support esp_system mc100_board esp_driver_i2s esp_driver_sdmmc esp_driver_gpio esp_adc esp_timer fatfs sdmmc
)
# The opt-in legacy source is compiled in this component to avoid a
# CONFIG-dependent component requirement. Keep its private header path off the
# IDF component dependency metadata so the default product graph stays clean.
target_include_directories(${COMPONENT_LIB} PRIVATE
                           "${CMAKE_CURRENT_LIST_DIR}/../mc100_supervisor/include")
target_compile_features(${COMPONENT_LIB} PRIVATE c_std_11)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
