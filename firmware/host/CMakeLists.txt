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

if(MSVC)
    target_compile_options(test_board_contract PRIVATE /W4 /WX /UNDEBUG)
else()
    target_compile_options(test_board_contract PRIVATE -Wall -Wextra -Werror -Wpedantic -UNDEBUG)
    if(MC100_ENABLE_SANITIZERS)
        target_compile_options(test_board_contract PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_board_contract PRIVATE -fsanitize=address,undefined)
    endif()
endif()

if(MSVC AND MC100_ENABLE_SANITIZERS)
    message(FATAL_ERROR "ASan/UBSan requires a supported GCC or Clang host compiler")
endif()

add_test(NAME board_contract COMMAND test_board_contract)
