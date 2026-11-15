#pragma once
#include "fake_lwip.h"
inline void cyw43_arch_lwip_begin() { ++fake::lock_depth; }
inline void cyw43_arch_lwip_end() { --fake::lock_depth; }
