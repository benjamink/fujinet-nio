cmake_minimum_required(VERSION 3.21)
project(pico_sd_tests LANGUAGES C CXX)
enable_testing()

# The SD card stack against a simulated card: the real SPI-mode driver and the real FatFs (the
# copy in the Pico SDK's TinyUSB, built with the firmware's ffconf.h). Needs PICO_SDK_PATH.
if(NOT DEFINED ENV{PICO_SDK_PATH})
    message(FATAL_ERROR "Set PICO_SDK_PATH to the Pico SDK the firmware is built with (FatFs lives in its TinyUSB)")
endif()
set(NIO ${CMAKE_CURRENT_LIST_DIR}/../..)
include(${NIO}/pico2w/fatfs/fatfs.cmake)
nio_fatfs_stage($ENV{PICO_SDK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/fatfs)

add_library(nio_fatfs STATIC ${NIO_FATFS_SOURCES})
target_include_directories(nio_fatfs PUBLIC ${NIO_FATFS_INCLUDE_DIR})
target_compile_options(nio_fatfs PRIVATE -w)

add_executable(pico-sd-tests
    test_sd_card.cpp
    test_sd_filesystem.cpp
    ${NIO}/src/platform/pico/sd_card.cpp
    ${NIO}/src/platform/pico/sd_filesystem.cpp
    ${NIO}/src/lib/fs/directory_range.cpp)
target_compile_features(pico-sd-tests PRIVATE cxx_std_20)
target_include_directories(pico-sd-tests PRIVATE .. ../../include ../../src/platform/pico)
target_compile_options(pico-sd-tests PRIVATE -Wall -Wextra -Werror)
target_link_libraries(pico-sd-tests PRIVATE nio_fatfs)
add_test(NAME pico-sd-tests COMMAND pico-sd-tests)
