#pragma once
#include <cstdint>
inline std::uint64_t test_monotonic_us=0;
inline std::uint64_t time_us_64() { return test_monotonic_us; }
