cmake_minimum_required(VERSION 3.21)
project(pico_http_tests LANGUAGES CXX)
enable_testing()
add_executable(pico-http-tests test_http_protocol.cpp ../test_pico_http_parser.cpp ../test_pico_http_request.cpp
    ../../src/platform/pico/http_protocol.cpp)
target_compile_features(pico-http-tests PRIVATE cxx_std_20)
target_include_directories(pico-http-tests PRIVATE .. ../../include)
target_compile_options(pico-http-tests PRIVATE -Wall -Wextra -Werror)
add_test(NAME pico-http-tests COMMAND pico-http-tests)
