cmake_minimum_required(VERSION 3.22)
project(mc100_host_tests LANGUAGES C)

include(CTest)

option(MC100_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(MC100_ENABLE_FUTURE_RUNTIME_TESTS "Enable retained runtime integration tests" OFF)

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

if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
    add_library(mc100_core STATIC
        ../components/mc100_core/state.c
        ../components/mc100_core/battery_policy.c
        ../components/mc100_core/upload_noop.c
    )
    target_include_directories(mc100_core PUBLIC ../components/mc100_core/include)
    mc100_host_checks(mc100_core)
    foreach(name state battery_policy)
        add_executable(test_${name} ../tests/test_${name}.c)
        target_link_libraries(test_${name} PRIVATE mc100_core)
        mc100_host_checks(test_${name})
        add_test(NAME ${name} COMMAND test_${name})
    endforeach()
    add_executable(test_upload_noop ../tests/test_upload_noop.c)
    target_link_libraries(test_upload_noop PRIVATE mc100_core)
    mc100_host_checks(test_upload_noop)
    add_test(NAME upload_noop COMMAND test_upload_noop)
endif()

set(mc100_audio_sources
    ../components/mc100_audio/frame_assembler.c
    ../components/mc100_audio/pcm_filter.c)
if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
    list(APPEND mc100_audio_sources
        ../components/mc100_audio/audio.c
        ../components/mc100_audio/preroll.c
        ../components/mc100_audio/stream.c
        ../components/mc100_audio/vad_fixed.c
    )
endif()
add_library(mc100_audio STATIC ${mc100_audio_sources})
target_include_directories(mc100_audio PUBLIC
    ../components/mc100_audio/include
    ../components/mc100_core/include
)
mc100_host_checks(mc100_audio)
add_library(mc100_recorder STATIC
    ../components/mc100_recorder/record_session.c)
target_include_directories(mc100_recorder PUBLIC
    ../components/mc100_recorder/include
    ../components/mc100_core/include)
mc100_host_checks(mc100_recorder)
add_executable(test_record_session ../tests/test_record_session.c)
target_link_libraries(test_record_session PRIVATE mc100_recorder)
mc100_host_checks(test_record_session)
add_test(NAME record_session COMMAND test_record_session)
add_executable(test_frame_assembler ../tests/test_frame_assembler.c)
target_link_libraries(test_frame_assembler PRIVATE mc100_audio)
mc100_host_checks(test_frame_assembler)
add_test(NAME frame_assembler COMMAND test_frame_assembler)
add_executable(test_pcm_filter ../tests/test_pcm_filter.c)
target_link_libraries(test_pcm_filter PRIVATE mc100_audio)
mc100_host_checks(test_pcm_filter)
add_test(NAME pcm_filter COMMAND test_pcm_filter)
add_executable(test_record_assembler_gap ../tests/test_record_assembler_gap.c)
target_link_libraries(test_record_assembler_gap PRIVATE mc100_audio)
mc100_host_checks(test_record_assembler_gap)
add_test(NAME record_assembler_gap COMMAND test_record_assembler_gap)
if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
foreach(name preroll_handoff queue_full capture_gap)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_audio)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
add_executable(test_vad_fixed ../tests/test_vad_fixed.c)
target_link_libraries(test_vad_fixed PRIVATE mc100_audio)
mc100_host_checks(test_vad_fixed)
add_test(NAME vad_fixed COMMAND test_vad_fixed)
endif()

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

if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
add_library(mc100_supervisor STATIC
    ../components/mc100_supervisor/supervisor.c
)
target_include_directories(mc100_supervisor PUBLIC
    ../components/mc100_supervisor/include
)
target_link_libraries(mc100_supervisor PUBLIC mc100_core mc100_audio mc100_writer)
mc100_host_checks(mc100_supervisor)
add_executable(test_supervisor_boot ../tests/test_supervisor_boot.c)
target_include_directories(test_supervisor_boot PRIVATE
    ../components/mc100_supervisor/include
)
target_link_libraries(test_supervisor_boot PRIVATE mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_boot)
add_test(NAME supervisor_boot COMMAND test_supervisor_boot)
add_executable(test_supervisor_record_open ../tests/test_supervisor_record_open.c)
target_include_directories(test_supervisor_record_open PRIVATE
    ../components/mc100_supervisor/include
)
target_link_libraries(test_supervisor_record_open PRIVATE mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_record_open)
add_test(NAME supervisor_record_open COMMAND test_supervisor_record_open)
add_executable(test_supervisor_record_active ../tests/test_supervisor_record_active.c)
target_include_directories(test_supervisor_record_active PRIVATE
    ../components/mc100_supervisor/include
)
target_link_libraries(test_supervisor_record_active PRIVATE mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_record_active)
add_test(NAME supervisor_record_active COMMAND test_supervisor_record_active)
add_executable(test_supervisor_edges ../tests/test_supervisor_edges.c)
target_include_directories(test_supervisor_edges PRIVATE
    ../components/mc100_supervisor/include)
target_link_libraries(test_supervisor_edges PRIVATE mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_edges)
add_test(NAME supervisor_edges COMMAND test_supervisor_edges)
add_executable(test_supervisor_monitor_events
    ../tests/test_supervisor_monitor_events.c)
target_include_directories(test_supervisor_monitor_events PRIVATE
    ../components/mc100_supervisor/include)
target_link_libraries(test_supervisor_monitor_events PRIVATE
    mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_monitor_events)
add_test(NAME supervisor_monitor_events COMMAND test_supervisor_monitor_events)
add_executable(test_supervisor_rearm ../tests/test_supervisor_rearm.c)
target_include_directories(test_supervisor_rearm PRIVATE
    ../components/mc100_supervisor/include)
target_link_libraries(test_supervisor_rearm PRIVATE
    mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_rearm)
add_test(NAME supervisor_rearm COMMAND test_supervisor_rearm)
add_executable(test_supervisor_audio_owner
    ../tests/test_supervisor_audio_owner.c)
target_include_directories(test_supervisor_audio_owner PRIVATE
    ../components/mc100_supervisor/include)
target_link_libraries(test_supervisor_audio_owner PRIVATE
    mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_audio_owner)
add_test(NAME supervisor_audio_owner COMMAND test_supervisor_audio_owner)
foreach(name supervisor_close supervisor_fault supervisor_recover_roundtrip)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_include_directories(test_${name} PRIVATE
        ../components/mc100_supervisor/include
    )
    target_link_libraries(test_${name} PRIVATE mc100_supervisor mc100_fake_io)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
add_executable(test_supervisor_rotation_publication
    ../tests/test_supervisor_rotation_publication.c)
target_include_directories(test_supervisor_rotation_publication PRIVATE
    ../components/mc100_supervisor/include)
target_link_libraries(test_supervisor_rotation_publication PRIVATE
    mc100_supervisor mc100_fake_io)
mc100_host_checks(test_supervisor_rotation_publication)
add_test(NAME supervisor_rotation_publication
    COMMAND test_supervisor_rotation_publication)
endif()
foreach(name storage_writer storage_short_write rotate_15000 space_admission finalize_faults)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_fake_io)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
add_executable(test_writer_publication ../tests/test_writer_publication.c)
target_link_libraries(test_writer_publication PRIVATE mc100_fake_io)
mc100_host_checks(test_writer_publication)
add_test(NAME writer_publication COMMAND test_writer_publication)
if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
foreach(name recover_prefix recover_idempotent)
    add_executable(test_${name} ../tests/test_${name}.c)
    target_link_libraries(test_${name} PRIVATE mc100_fake_io)
    mc100_host_checks(test_${name})
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
endif()
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
if(MC100_ENABLE_FUTURE_RUNTIME_TESTS)
add_executable(test_product_audio_protocol
    ../tests/test_product_audio_protocol.c)
target_include_directories(test_product_audio_protocol PRIVATE
    ../components/mc100_core/include)
mc100_host_checks(test_product_audio_protocol)
add_test(NAME product_audio_protocol COMMAND test_product_audio_protocol)
add_executable(test_evt_commands ../tests/test_evt_commands.c ../main/evt_commands.c)
target_include_directories(test_evt_commands PRIVATE ../main)
target_link_libraries(test_evt_commands PRIVATE mc100_audio mc100_writer)
mc100_host_checks(test_evt_commands)
add_test(NAME evt_commands COMMAND test_evt_commands)
endif()
add_test(NAME com7_client COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_com7_client.py)
add_test(NAME recording_verifier COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_recording_verifier.py)
