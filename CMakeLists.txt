cmake_minimum_required(VERSION 3.16)
project(stm32-bootloader C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_executable(
    test_crc32
    tests/test_crc32.c
)

add_executable(
    test_frame
    tests/test_frame.c
)

add_test(NAME crc32_tests COMMAND test_crc32)
add_test(NAME frame_tests COMMAND test_frame)

add_library(
    bl_core
    core/src/bl_crc32.c
    core/src/bl_frame.c
)

add_library(
    unity
    third_party/unity/unity.c
)

target_compile_options(bl_core
    PRIVATE
      -Wall
      -Wextra
      -Werror
      -fsanitize=address,undefined
)

target_compile_options(test_crc32
    PRIVATE
      -Wall
      -Wextra
      -Werror
      -fsanitize=address,undefined
)

target_link_options(test_crc32
    PRIVATE
      -fsanitize=address,undefined
)

target_link_libraries(test_crc32
    PRIVATE
      bl_core
      unity
)

target_compile_options(test_frame
    PRIVATE
      -Wall
      -Wextra
      -Werror
      -fsanitize=address,undefined
)

target_link_options(test_frame
    PRIVATE
      -fsanitize=address,undefined
)

target_link_libraries(test_frame
    PRIVATE
      bl_core
      unity
)

target_include_directories(bl_core
    PUBLIC
      core/include
)

target_include_directories(unity
    PUBLIC
      third_party/unity/
)
