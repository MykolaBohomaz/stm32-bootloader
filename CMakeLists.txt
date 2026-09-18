cmake_minimum_required(VERSION 3.16)

project(stm32-bootloader
    VERSION 0.1.0
    DESCRIPTION "Portable STM32 bootloader with A/B slots and rollback"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Build configuration
#
# The default (host) build compiles bl_core for this machine, links it against
# the simulated port in port/host, and runs the unit tests.
#
# Configuring with -DBL_BUILD_TARGET=ON selects the firmware build: no tests,
# no sanitizers, no simulated port. That configuration requires the toolchain
# file in cmake/arm-none-eabi.cmake and is not yet complete.
# ---------------------------------------------------------------------------
option(BL_BUILD_TARGET "Build firmware for the STM32 target instead of host tests" OFF)

# Warning flags apply to first-party code in every configuration.
#
# -Wshadow is included deliberately: it is not implied by -Wall or -Wextra,
# and a shadowed variable has already caused a silently inert test in this
# project.
#
# -Wconversion and -Wsign-conversion matter on a bootloader that packs and
# unpacks untrusted length and CRC fields: they force every narrowing
# conversion to be written out explicitly rather than happening silently.
set(BL_WARNING_FLAGS
    -Wall
    -Wextra
    -Werror
    -Wshadow
    -Wconversion
    -Wsign-conversion
)

# Sanitizers are host-only. AddressSanitizer requires a runtime and shadow
# memory that do not exist on a Cortex-M target.
set(BL_SANITIZER_FLAGS
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
)

# ---------------------------------------------------------------------------
# bl_core - hardware-independent bootloader logic
#
# Compiles unchanged for both the host and the STM32. Must never include a
# hardware header; CI enforces this.
# ---------------------------------------------------------------------------
add_library(bl_core STATIC
    core/src/bl_crc32.c
    core/src/bl_frame.c
    core/src/bl_meta.c
    core/src/bl_core.c
    core/src/bl_api.c
)

target_include_directories(bl_core
    PUBLIC
        core/include
)

target_compile_options(bl_core
    PRIVATE
        ${BL_WARNING_FLAGS}
)

if(NOT BL_BUILD_TARGET)
    target_compile_options(bl_core PUBLIC ${BL_SANITIZER_FLAGS})
    target_link_options(bl_core PUBLIC ${BL_SANITIZER_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Host-only targets: simulated port, test framework, unit tests
# ---------------------------------------------------------------------------
if(NOT BL_BUILD_TARGET)

    enable_testing()

    # --- Simulated port -----------------------------------------------------
    #
    # Deliberately a separate library from bl_core: it contains a 256 KiB
    # flash array and test hooks that must never reach firmware.
    add_library(bl_port_host STATIC
        port/host/bl_port_host.c
    )

    target_include_directories(bl_port_host
        PUBLIC
            port/host
    )

    target_link_libraries(bl_port_host
        PUBLIC
            bl_core
    )

    target_compile_options(bl_port_host
        PRIVATE
            ${BL_WARNING_FLAGS}
    )

    # --- Unity --------------------------------------------------------------
    #
    # Vendored third-party code: built without the project warning flags so
    # upstream changes cannot break our build.
    add_library(unity STATIC
        third_party/unity/unity.c
    )

    target_include_directories(unity
        PUBLIC
            third_party/unity
    )

    if(NOT BL_BUILD_TARGET)
        target_compile_options(unity PUBLIC ${BL_SANITIZER_FLAGS})
        target_link_options(unity PUBLIC ${BL_SANITIZER_FLAGS})
    endif()

    # --- Tests --------------------------------------------------------------
    #
    # bl_add_test(<name>) builds tests/test_<name>.c and registers it with
    # CTest. Adding a test file is then a single line.
    function(bl_add_test test_name)
        add_executable(test_${test_name} tests/test_${test_name}.c)

        target_link_libraries(test_${test_name}
            PRIVATE
                bl_port_host
                unity
        )

        target_compile_options(test_${test_name}
            PRIVATE
                ${BL_WARNING_FLAGS}
        )

        add_test(NAME ${test_name} COMMAND test_${test_name})
    endfunction()

    bl_add_test(crc32)
    bl_add_test(frame)
    bl_add_test(port_host)
    bl_add_test(meta)
    bl_add_test(core)
    bl_add_test(session)
    bl_add_test(api)

    # --- Simulated device shared library ------------------------------------
    #
    # Lets tools/blflash.py be exercised against the real bootloader core
    # instead of a Python reimplementation of it. The protocol has no
    # external specification, so driving the actual firmware logic is the
    # only way to confirm the two implementations agree.
    #
    # Built from the same sources as the test targets but deliberately
    # WITHOUT sanitizers: the library is loaded into a plain Python process
    # via ctypes, and the AddressSanitizer runtime cannot be introduced by
    # dlopen after the process has started.
    add_library(bl_sim SHARED
        core/src/bl_crc32.c
        core/src/bl_frame.c
        core/src/bl_meta.c
        core/src/bl_core.c
        core/src/bl_api.c
        port/host/bl_port_host.c
        sim/bl_sim.c
    )

    target_include_directories(bl_sim
        PRIVATE
            core/include
            port/host
    )

    target_compile_options(bl_sim
        PRIVATE
            ${BL_WARNING_FLAGS}
    )

    set_target_properties(bl_sim PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )

    # --- Python tests -------------------------------------------------------
    #
    # Registered with CTest so a single `ctest` run covers both sides of
    # the protocol.
    find_package(Python3 COMPONENTS Interpreter)

    if(Python3_FOUND)
        add_test(
            NAME python_tools
            COMMAND ${Python3_EXECUTABLE} -m unittest discover
                    -s ${CMAKE_SOURCE_DIR}/tests -p "test_*.py" -v
        )

        set_tests_properties(python_tools PROPERTIES
            ENVIRONMENT
                "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools;BL_SIM_LIB=$<TARGET_FILE:bl_sim>"
        )

        add_test(
            NAME constants_in_sync
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tools/check_sync.py
                    ${CMAKE_SOURCE_DIR}
        )
    endif()

endif()
