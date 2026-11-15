#pragma once
namespace fujinet::platform::pico {
// Core 0 boot only. False forbids proceeding to flash initialization.
bool start_usb_worker();
void service_console();
}
