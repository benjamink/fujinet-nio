#pragma once
namespace fujinet::platform::pico {
// Core 0 only. Copies raw socket observations under the lwIP lock, then emits
// at most one bounded UART line per second OUTSIDE that lock. Ordinary traffic
// is silent even without FN_DEBUG. An optional sink supports native verification.
void service_tcp_diagnostics(void (*emit)(const char*) = nullptr);
}
