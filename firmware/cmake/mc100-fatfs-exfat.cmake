# This is intentionally applied after project(): the actual component target
# exists, and its PUBLIC requirements reach all linked FatFs consumers.
idf_build_get_property(mc100_idf_path IDF_PATH)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../dependencies.lock.json" mc100_dependency_lock)
string(JSON mc100_locked_revision GET "${mc100_dependency_lock}" esp_idf commit)
execute_process(COMMAND git -C "${mc100_idf_path}" rev-parse HEAD
    RESULT_VARIABLE mc100_revision_result OUTPUT_VARIABLE mc100_actual_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT mc100_revision_result EQUAL 0 OR NOT mc100_actual_revision STREQUAL mc100_locked_revision)
    message(FATAL_ERROR "MC100 exFAT overlay requires the exact dependencies.lock.json SDK revision")
endif()
# Also reject a modified ffconf.h on that revision. Other SDK configuration and
# tool tuple checks remain owned by tools/build.ps1.
execute_process(COMMAND git -C "${mc100_idf_path}" diff --quiet HEAD -- components/fatfs/src/ffconf.h
    RESULT_VARIABLE mc100_ffconf_clean)
if(NOT mc100_ffconf_clean EQUAL 0)
    message(FATAL_ERROR "MC100 exFAT overlay requires the original SDK ffconf.h")
endif()
idf_component_get_property(mc100_fatfs_library fatfs COMPONENT_LIB)
target_compile_options(${mc100_fatfs_library} PUBLIC
    "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include \"${CMAKE_CURRENT_LIST_DIR}/mc100-fatfs-exfat.h\">")
# An actual compiler assertion, built with the same options as ff.c, catches
# accidental loss of the overlay or a mismatched file-size ABI during build.
target_sources(${mc100_fatfs_library} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../tests/test_fatfs_exfat_abi.c")
