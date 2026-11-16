# FujiNet NIO on the Raspberry Pi Pico 2 W (RP2350 + CYW43439 Wi-Fi).
#
# An independent Pico SDK project, like bridges/*: the ESP-IDF and POSIX builds
# do not include it. It compiles the portable core (src/lib) plus the Pico
# platform layer (src/platform/pico). See pico2w/README.md for what is real and
# build measurements and hardware acceptance limits.
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
# Platform trust: the Mozilla roots, packed by tools/make_ca_bundle.py into a flash bundle that
# the device searches by issuer name (see src/platform/pico/detail/ca_bundle.h). NIO_PICO_CA_DATE
# is the snapshot date of the PEM; roots the device's mbedTLS cannot use, or that had expired by
# then, are dropped at build time, so the same inputs always give the same bundle.
set(NIO_PICO_CA_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/certs/mozilla-roots.pem" CACHE FILEPATH "PEM platform CA roots")
set(NIO_PICO_CA_DATE "2026-11-05" CACHE STRING "Snapshot date of the CA PEM, YYYY-MM-DD")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(NIO_CA_BUNDLE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ca_bundle_data.h")
add_custom_command(
    OUTPUT "${NIO_CA_BUNDLE_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_LIST_DIR}/tools/make_ca_bundle.py"
            "${NIO_PICO_CA_BUNDLE}" --header "${NIO_CA_BUNDLE_HEADER}" --now "${NIO_PICO_CA_DATE}"
    DEPENDS "${NIO_PICO_CA_BUNDLE}" "${CMAKE_CURRENT_LIST_DIR}/tools/make_ca_bundle.py"
    COMMENT "Packing the trusted-root bundle"
    VERBATIM)
# FN_LOG* compile to nothing unless FN_DEBUG is defined (as on POSIX Debug and ESP32
# debug builds). Turn it on to get logs and the boot banner on UART0 (GPIO0 TX).
option(FN_DEBUG "Compile FN_LOG* to UART0 stdio" OFF)
# CYW43 Wi-Fi power save. The driver default sleeps the radio between beacons, which added
# 70 to 230 ms to every round trip on hardware (ping 150 ms against 1.3 ms). This adapter is
# USB powered, so the radio stays awake; choose another mode for a special case.
set(NIO_PICO_WIFI_PM "none" CACHE STRING "CYW43 power save: none, default, performance or aggressive")
set_property(CACHE NIO_PICO_WIFI_PM PROPERTY STRINGS none default performance aggressive)
string(TOUPPER "${NIO_PICO_WIFI_PM}" NIO_WIFI_PM_UP)
if(NOT NIO_WIFI_PM_UP MATCHES "^(DEFAULT|PERFORMANCE|AGGRESSIVE|NONE)$")
    message(FATAL_ERROR "NIO_PICO_WIFI_PM must be default, performance, aggressive or none")
endif()
option(FN_HTTPS_TEST_CA "Trust only FujiNet test CA" OFF)
option(FN_HTTPS_TEST_CA_ADDITIVE "Add FujiNet test CA to platform roots" OFF)
if(FN_HTTPS_TEST_CA AND FN_HTTPS_TEST_CA_ADDITIVE)
    message(FATAL_ERROR "Choose only one test CA trust mode")
endif()
# SDK 2.3.1 entropy polling overruns a final non-eight-byte chunk. Own this
# callback with a remaining-length copy; hardware SHA ALT is not enabled.
get_target_property(NIO_MBEDTLS_SOURCES pico_mbedtls INTERFACE_SOURCES)
list(FILTER NIO_MBEDTLS_SOURCES EXCLUDE REGEX "/pico_mbedtls\.c$")
set_property(TARGET pico_mbedtls PROPERTY INTERFACE_SOURCES "${NIO_MBEDTLS_SOURCES}")

# The firmware is built for size (-Os), but an HTTPS handshake spends nearly all its time in mbedTLS's
# big-number and elliptic-curve arithmetic, and record encryption sets transfer speed. Build those
# files for speed. Set NIO_PICO_MBEDTLS_OPT to "" to build them for size like everything else.
# SD card on SPI (pico2w/README.md, "SD card"). Defaults: SPI0 on GP16 MISO, GP17 CS, GP18 SCK,
# GP19 MOSI (physical pins 21, 22, 24, 25, with GND on 23). Any other pins valid for the chosen
# SPI block work; the 10 MHz clock suits breadboard wiring (cards allow up to 25 MHz).
set(NIO_PICO_SD_SPI 0 CACHE STRING "SPI block (0 or 1) the SD card is wired to")
set(NIO_PICO_SD_MISO 16 CACHE STRING "SD card MISO (DO) GPIO")
set(NIO_PICO_SD_CS 17 CACHE STRING "SD card chip select GPIO")
set(NIO_PICO_SD_SCK 18 CACHE STRING "SD card clock GPIO")
set(NIO_PICO_SD_MOSI 19 CACHE STRING "SD card MOSI (DI) GPIO")
set(NIO_PICO_SD_BAUD 10000000 CACHE STRING "SD card SPI clock in Hz after initialisation")

include(${CMAKE_CURRENT_LIST_DIR}/fatfs/fatfs.cmake)
nio_fatfs_stage(${PICO_SDK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/fatfs)

set(NIO_PICO_MBEDTLS_OPT "-O2" CACHE STRING "Optimisation flag for mbedTLS's crypto arithmetic (empty: build for size)")
set(NIO_MBEDTLS_FAST_FILES bignum.c bignum_core.c bignum_mod.c bignum_mod_raw.c ecp.c ecp_curves.c
    ecdsa.c ecdh.c aes.c gcm.c sha256.c sha512.c constant_time.c)
if(NIO_PICO_MBEDTLS_OPT)
    # The library's sources are INTERFACE_SOURCES of these SDK targets and are compiled into the firmware.
    set(NIO_MBEDTLS_FAST_COUNT 0)
    foreach(target pico_mbedtls_crypto pico_mbedtls_x509 pico_mbedtls_tls)
        get_target_property(sources ${target} INTERFACE_SOURCES)
        foreach(src ${sources})
            get_filename_component(name "${src}" NAME)
            if(name IN_LIST NIO_MBEDTLS_FAST_FILES)
                set_source_files_properties("${src}" PROPERTIES COMPILE_OPTIONS "${NIO_PICO_MBEDTLS_OPT}")
                math(EXPR NIO_MBEDTLS_FAST_COUNT "${NIO_MBEDTLS_FAST_COUNT} + 1")
            endif()
        endforeach()
    endforeach()
    if(NIO_MBEDTLS_FAST_COUNT EQUAL 0)
        message(FATAL_ERROR "NIO_PICO_MBEDTLS_OPT matched no mbedTLS source: the SDK's target layout changed")
    endif()
    message(STATUS "mbedTLS: ${NIO_MBEDTLS_FAST_COUNT} files built with ${NIO_PICO_MBEDTLS_OPT}")
endif()

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
list(FILTER NIO_LIB_SOURCES EXCLUDE REGEX "/src/lib/(diagnostic_hotsync_provider|diagnostic_uart_channel_provider|fs_stdio)\\.cpp$")

file(GLOB NIO_PICO_PLATFORM_SOURCES CONFIGURE_DEPENDS ${NIO}/src/platform/pico/*.cpp)
# Portable stdio logging remains shared with POSIX. Pico owns its clock backend.
list(APPEND NIO_PICO_PLATFORM_SOURCES
    ${NIO}/src/platform/posix/logging.cpp)

# Core 0 owns NIO/network; the fixed core 1 worker owns USB.
add_executable(fujinet_nio_pico2w
    ${NIO_CA_BUNDLE_HEADER}
    ${NIO}/src/app/main_pico.cpp
    ${NIO}/src/app/console_engine.cpp
    ${NIO}/src/app/console_commands.cpp
    ${NIO}/src/app/console_parse.cpp
    ${NIO}/src/app/console_fs_shell.cpp
    ${NIO}/src/lib/build_profile/rp2350_pico2w.cpp
    ${NIO}/src/lib/hotsync_init_off.cpp
    ${NIO}/third_party/esp/esp_littlefs/src/littlefs/lfs.c
    ${NIO}/third_party/esp/esp_littlefs/src/littlefs/lfs_util.c
    ${NIO}/third_party/cjson/cJSON.c
    ${NIO_FATFS_SOURCES}
    ${NIO}/third_party/cjson/cJSON_Utils.c
    ${NIO_LIB_SOURCES}
    ${NIO_PICO_PLATFORM_SOURCES}
)

target_include_directories(fujinet_nio_pico2w PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${NIO_FATFS_INCLUDE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}            # lwipopts.h, mbedtls_config.h
    ${CMAKE_CURRENT_LIST_DIR}/compat     # <mutex>, <condition_variable> for newlib
    ${NIO}/include
    ${NIO}/src/lib
    ${NIO}/third_party
    ${NIO}/third_party/stb
    ${NIO}/third_party/cjson
    ${NIO}/third_party/esp/esp_littlefs/src/littlefs
)
target_compile_definitions(fujinet_nio_pico2w PRIVATE
    FN_HTTPS_TEST_CA=$<BOOL:${FN_HTTPS_TEST_CA}>
    FN_HTTPS_TEST_CA_ADDITIVE=$<BOOL:${FN_HTTPS_TEST_CA_ADDITIVE}>
    NIO_PICO_SD_SPI=${NIO_PICO_SD_SPI}
    NIO_PICO_SD_MISO=${NIO_PICO_SD_MISO}
    NIO_PICO_SD_CS=${NIO_PICO_SD_CS}
    NIO_PICO_SD_SCK=${NIO_PICO_SD_SCK}
    NIO_PICO_SD_MOSI=${NIO_PICO_SD_MOSI}
    NIO_PICO_SD_BAUD=${NIO_PICO_SD_BAUD}
    PICO_STACK_SIZE=16384
    PICO_CORE1_STACK_SIZE=4096
    PICO_MULTICORE_LOCKOUT_BEFORE_CORE1_STARTED=0
    FN_PLATFORM_PICO
    NIO_PICO_WIFI_PM=CYW43_${NIO_WIFI_PM_UP}_PM
    $<$<BOOL:${FN_DEBUG}>:FN_DEBUG>
    PICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1
)
target_compile_options(fujinet_nio_pico2w PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-psabi>
    -ffunction-sections -fdata-sections
)
# TLS callbacks share core 0's stack. Reserve 16KiB at the top of main SRAM
# using SDK memory-location overrides; the default scratch-Y bank is only 4KiB.
target_link_options(fujinet_nio_pico2w PRIVATE
    -Wl,--defsym=RAM_LENGTH=507904
    -Wl,--defsym=SCRATCH_Y_ORIGIN=0x2007c000
    -Wl,--defsym=SCRATCH_Y_LENGTH=16384
    -Wl,-T,${CMAKE_CURRENT_LIST_DIR}/flash_limit.ld -Wl,--gc-sections -Wl,-Map=$<TARGET_FILE:fujinet_nio_pico2w>.map)
target_link_libraries(fujinet_nio_pico2w PRIVATE
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mbedtls
    pico_rand
    pico_mbedtls
    pico_flash
    pico_multicore
    hardware_flash
    hardware_spi
    hardware_watchdog
    yaml-cpp
)
pico_enable_stdio_usb(fujinet_nio_pico2w 0)
pico_enable_stdio_uart(fujinet_nio_pico2w 1)
pico_add_extra_outputs(fujinet_nio_pico2w)

# Size report: flash and static RAM of the gc-sections link.
add_custom_command(TARGET fujinet_nio_pico2w POST_BUILD
    COMMAND ${PICO_GCC_TRIPLE}-size $<TARGET_FILE:fujinet_nio_pico2w>
    VERBATIM)
