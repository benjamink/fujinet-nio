cmake_minimum_required(VERSION 3.21)
project(pico_ca_tests LANGUAGES C CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(NIO ${CMAKE_CURRENT_LIST_DIR}/../..)
set(ROOTS ${NIO}/pico2w/certs/mozilla-roots.pem)
set(GEN ${NIO}/pico2w/tools/make_ca_bundle.py)
set(BUNDLE_H ${CMAKE_CURRENT_BINARY_DIR}/gen/ca_bundle_data.h)

# The same inputs the firmware uses, packed the same way.
add_custom_command(
    OUTPUT ${BUNDLE_H}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND ${Python3_EXECUTABLE} -I ${GEN} ${ROOTS} --header ${BUNDLE_H} --now 2026-11-05
    DEPENDS ${ROOTS} ${GEN} VERBATIM)

# 1. The bundle format and lookup: no mbedTLS needed.
add_executable(pico-ca-bundle-tests test_ca_bundle.cpp ${BUNDLE_H})
target_compile_features(pico-ca-bundle-tests PRIVATE cxx_std_20)
target_include_directories(pico-ca-bundle-tests PRIVATE .. ../../src/platform/pico ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_options(pico-ca-bundle-tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME pico-ca-bundle-tests COMMAND pico-ca-bundle-tests)

# 2. The firmware's trust code against the real mbedTLS, built with the firmware's own
#    configuration, verifying real certificate chains. Needs the Pico SDK's mbedTLS sources.
set(MBED $ENV{PICO_SDK_PATH}/lib/mbedtls)
if(DEFINED ENV{PICO_SDK_PATH} AND EXISTS ${MBED}/library/x509_crt.c)
    file(GLOB MBED_SOURCES ${MBED}/library/*.c)
    add_library(nio_mbedtls STATIC ${MBED_SOURCES})
    target_include_directories(nio_mbedtls PUBLIC ${MBED}/include ${MBED}/library)
    target_compile_definitions(nio_mbedtls PUBLIC MBEDTLS_CONFIG_FILE="${NIO}/pico2w/mbedtls_config.h")
    target_compile_options(nio_mbedtls PRIVATE -w)

    add_executable(pico-ca-verify-tests test_ca_verify.cpp ${NIO}/src/platform/pico/tls_trust.cpp ${BUNDLE_H})
    target_compile_features(pico-ca-verify-tests PRIVATE cxx_std_20)
    target_include_directories(pico-ca-verify-tests PRIVATE .. ../../src/platform/pico ${CMAKE_CURRENT_BINARY_DIR}/gen ../../include)
    target_compile_definitions(pico-ca-verify-tests PRIVATE FIXTURES_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")
    target_compile_options(pico-ca-verify-tests PRIVATE -Wall -Wextra -Werror)
    target_link_libraries(pico-ca-verify-tests PRIVATE nio_mbedtls)
    add_test(NAME pico-ca-verify-tests COMMAND pico-ca-verify-tests)
else()
    message(WARNING "PICO_SDK_PATH not set (or no mbedtls): skipping pico-ca-verify-tests")
endif()
