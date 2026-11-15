#pragma once
#include "fake_lwip.h"
using absolute_time_t = std::uint64_t;
inline absolute_time_t get_absolute_time() { return fake::now; }
inline std::uint64_t to_ms_since_boot(absolute_time_t t) { return t; }
inline absolute_time_t make_timeout_time_ms(std::uint32_t n) { return fake::now + n; }
inline bool time_reached(absolute_time_t t) { return fake::now >= t; }
inline void sleep_ms(std::uint32_t n) {
    if (fake::lock_depth) throw std::logic_error("sleep under lwIP lock");
    fake::now += n;
    auto callback = fake::on_sleep;
    if (callback) callback();
}
