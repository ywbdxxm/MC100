if(NOT CONFIG_MC100_APP_PRODUCT OR CONFIG_MC100_LEGACY_RUNTIME)
    idf_component_register(
        SRCS "state.c" "battery_policy.c" "upload_noop.c"
        INCLUDE_DIRS "include")
else()
    idf_component_register(INCLUDE_DIRS "include")
endif()
