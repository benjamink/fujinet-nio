# FujiNet NIO on the Raspberry Pi Pico 2 W (RP2350 + CYW43439 Wi-Fi).
#
# An independent Pico SDK project, like bridges/*: the ESP-IDF and POSIX builds
# do not include it. It compiles the portable core (src/lib) plus the Pico
# platform layer (src/platform/pico). See pico2w/README.md for what is real and
# what is still a stub.
#
#   source scripts/env.sh        # exports PICO_SDK_PATH
#   cmake -S pico2w -B build/pico2w && cmake --build build/pico2w
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH (source the workspace scripts/env.sh)")
endif()

set(PICO_BOARD pico2_w CACHE STRING "Board")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# Size matters on this part; the ESP32 build is -Os too.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(fujinet_nio_pico2w C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)   # gnu++20: newlib hides setenv/tzset/localtime_r under strict ISO
set(PICO_CXX_ENABLE_EXCEPTIONS 1) # the core and yaml-cpp throw and catch
set(PICO_CXX_ENABLE_RTTI 1)        # dynamic_cast on devices, as on ESP32
pico_sdk_init()

set(NIO ${CMAKE_CURRENT_LIST_DIR}/..)

# yaml-cpp, as for the POSIX build: library only.
set(YAML_CPP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS   OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${NIO}/third_party/yaml-cpp yaml-cpp EXCLUDE_FROM_ALL)

# The portable core. Everything under src/lib except what a Pico build cannot
# or must not take (each exclusion is listed, with its reason, in the README).
file(GLOB_RECURSE NIO_LIB_SOURCES CONFIGURE_DEPENDS ${NIO}/src/lib/*.cpp)
list(FILTER NIO_LIB_SOURCES EXCLUDE REGEX "/src/lib/(build_profile|visor_usb|hotsync)/")
list(FILTER NIO_LIB_SOURCES EXCLUDE REGEX "/src/lib/(diagnostic_hotsync_provider|diagnostic_uart_channel_provider|fs_stdio|udp_channel)\\.cpp$")

file(GLOB NIO_PICO_PLATFORM_SOURCES CONFIGURE_DEPENDS ${NIO}/src/platform/pico/*.cpp)
# Portable newlib code that lives under posix/ today (stdio logging, <ctime> time
# and timezone helpers). Shared, not copied; promote to src/lib if a third user appears.
list(APPEND NIO_PICO_PLATFORM_SOURCES
    ${NIO}/src/platform/posix/time.cpp
    ${NIO}/src/platform/posix/logging.cpp)

# Single-core measurement of the real thing: no <mutex>/<thread> in newlib.
add_executable(fujinet_nio_pico2w
    ${NIO}/src/app/main_pico.cpp
    ${NIO}/src/lib/build_profile/rp2350_pico2w.cpp
    ${NIO}/src/lib/hotsync_init_off.cpp
    ${NIO}/third_party/cjson/cJSON.c
    ${NIO}/third_party/cjson/cJSON_Utils.c
    ${NIO_LIB_SOURCES}
    ${NIO_PICO_PLATFORM_SOURCES}
)

target_include_directories(fujinet_nio_pico2w PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # lwipopts.h, mbedtls_config.h
    ${CMAKE_CURRENT_LIST_DIR}/compat     # <mutex>, <condition_variable> for newlib
    ${NIO}/include
    ${NIO}/src/lib
    ${NIO}/third_party
    ${NIO}/third_party/stb
    ${NIO}/third_party/cjson
)
target_compile_definitions(fujinet_nio_pico2w PRIVATE
    FN_PLATFORM_PICO
    PICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1
)
target_compile_options(fujinet_nio_pico2w PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>
    -ffunction-sections -fdata-sections
)
target_link_options(fujinet_nio_pico2w PRIVATE -Wl,--gc-sections -Wl,-Map=$<TARGET_FILE:fujinet_nio_pico2w>.map)
target_link_libraries(fujinet_nio_pico2w PRIVATE
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_mbedtls
    hardware_flash
    hardware_watchdog
    yaml-cpp
)
pico_enable_stdio_usb(fujinet_nio_pico2w 1)
pico_enable_stdio_uart(fujinet_nio_pico2w 0)
pico_add_extra_outputs(fujinet_nio_pico2w)

# Size report: flash and static RAM of the gc-sections link.
add_custom_command(TARGET fujinet_nio_pico2w POST_BUILD
    COMMAND ${PICO_GCC_TRIPLE}-size $<TARGET_FILE:fujinet_nio_pico2w>
    VERBATIM)
