cmake_minimum_required(VERSION 3.21)
project(pico_platform_tests LANGUAGES C CXX)
enable_testing()
add_executable(pico-raw-socket-tests test_raw_sockets.cpp)
target_compile_features(pico-raw-socket-tests PRIVATE cxx_std_20)
target_include_directories(pico-raw-socket-tests PRIVATE fakes .. ../../include)
target_compile_options(pico-raw-socket-tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME pico-raw-socket-tests COMMAND pico-raw-socket-tests)

set(LITTLEFS ../../third_party/esp/esp_littlefs/src/littlefs)
add_executable(pico-flash-tests test_flash.cpp
    ../../src/platform/pico/littlefs_volume.cpp
    ../../src/lib/fs/directory_range.cpp
    ${LITTLEFS}/lfs.c ${LITTLEFS}/lfs_util.c)
target_compile_features(pico-flash-tests PRIVATE cxx_std_20)
target_include_directories(pico-flash-tests PRIVATE .. ../../include ${LITTLEFS})
target_compile_definitions(pico-flash-tests PRIVATE LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR)
target_compile_options(pico-flash-tests PRIVATE -Wall -Wextra)
add_test(NAME pico-flash-tests COMMAND pico-flash-tests)

add_executable(pico-usb-worker-tests test_usb_worker.cpp
    ../../src/platform/pico/usb_service.cpp
    ../../src/platform/pico/channel_factory.cpp)
target_compile_features(pico-usb-worker-tests PRIVATE cxx_std_20)
target_include_directories(pico-usb-worker-tests PRIVATE usb_fakes ../../include)
target_compile_options(pico-usb-worker-tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
foreach(mode lifecycle flash-failure usb-failure timeout)
    add_test(NAME pico-usb-worker-${mode} COMMAND pico-usb-worker-tests ${mode})
endforeach()
