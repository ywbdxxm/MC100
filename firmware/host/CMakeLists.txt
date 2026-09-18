cmake_minimum_required(VERSION 3.22)
project(mc100_host_tests LANGUAGES C)

include(CTest)

option(MC100_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_executable(test_board_contract ../tests/test_board_contract.c)
target_compile_features(test_board_contract PRIVATE c_std_11)
target_include_directories(
    test_board_contract
    PRIVATE
        ../components/mc100_core/include
        ../components/mc100_board/include
)

if(MSVC AND MC100_ENABLE_SANITIZERS)
    message(FATAL_ERROR "ASan/UBSan requires a supported GCC or Clang host compiler")
endif()

function(mc100_host_checks target)
    target_compile_features(${target} PRIVATE c_std_11)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /UNDEBUG)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
        if(MC100_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

mc100_host_checks(test_board_contract)
add_test(NAME board_contract COMMAND test_board_contract)

add_library(mc100_format STATIC
    ../components/mc100_storage/crc32.c
    ../components/mc100_storage/wav.c
    ../components/mc100_storage/journal.c
)
target_include_directories(mc100_format PUBLIC
    ../components/mc100_storage/include
    ../components/mc100_core/include
)
mc100_host_checks(mc100_format)

foreach(name test_wav test_journal_codec emit_format_fixture)
    add_executable(${name} ../tests/${name}.c)
    target_link_libraries(${name} PRIVATE mc100_format)
    mc100_host_checks(${name})
endforeach()
add_test(NAME wav COMMAND test_wav)
add_test(NAME journal_codec COMMAND test_journal_codec
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/format_v1.hex
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/format_v1_header.hex
)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME wave_interop COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_wave_interop.py
    $<TARGET_FILE:emit_format_fixture>
)

add_library(mc100_core STATIC
    ../components/mc100_core/state.c
    ../components/mc100_core/battery_policy.c
)
target_include_directories(mc100_core PUBLIC ../components/mc100_core/include)
mc100_host_checks(mc100_core)
foreach(name state battery_policy)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_core)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

add_library(mc100_audio STATIC
    ../components/mc100_audio/audio.c
    ../components/mc100_audio/preroll.c
    ../components/mc100_audio/stream.c
    ../components/mc100_audio/frame_assembler.c
)
target_include_directories(mc100_audio PUBLIC
    ../components/mc100_audio/include
    ../components/mc100_core/include
)
mc100_host_checks(mc100_audio)
foreach(name frame_assembler preroll_handoff queue_full capture_gap)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_audio)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

add_library(mc100_writer STATIC
    ../components/mc100_storage/writer.c
    ../components/mc100_storage/recovery.c
)
target_link_libraries(mc100_writer PUBLIC mc100_format)
mc100_host_checks(mc100_writer)
add_library(mc100_fake_io STATIC ../host/fake_io.c)
target_include_directories(mc100_fake_io PUBLIC ../host/include)
target_link_libraries(mc100_fake_io PUBLIC mc100_writer)
mc100_host_checks(mc100_fake_io)
foreach(name storage_writer storage_short_write rotate_15000 space_admission finalize_faults)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_fake_io)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
foreach(name recover_prefix recover_idempotent)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_fake_io)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
if(WIN32)
    add_library(mc100_file_io STATIC ../host/file_io.c)
    target_include_directories(mc100_file_io PUBLIC ../host/include)
    target_link_libraries(mc100_file_io PUBLIC mc100_writer)
    mc100_host_checks(mc100_file_io)
endif()
add_executable(test_driver_contract ../tests/test_driver_contract.c)
target_include_directories(test_driver_contract PRIVATE
    ../components/mc100_platform_espidf/include
    ../components/mc100_board/include
    ../components/mc100_core/include
)
mc100_host_checks(test_driver_contract)
add_test(NAME driver_contract COMMAND test_driver_contract)
add_executable(test_evt_commands ../tests/test_evt_commands.c ../main/evt_commands.c)
target_include_directories(test_evt_commands PRIVATE ../main)
target_link_libraries(test_evt_commands PRIVATE mc100_audio mc100_writer)
mc100_host_checks(test_evt_commands)
add_test(NAME evt_commands COMMAND test_evt_commands)
add_test(NAME com7_client COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_com7_client.py)
add_test(NAME recording_verifier COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_recording_verifier.py)
