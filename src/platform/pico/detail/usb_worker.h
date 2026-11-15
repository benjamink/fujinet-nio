#pragma once
#include "usb_bridge.h"
#include "pico/mutex.h"
namespace fujinet::platform::pico::detail {
struct UsbMutex {
    mutex_t mutex;
    UsbMutex() { mutex_init(&mutex); }
    void lock() { mutex_enter_blocking(&mutex); }
    void unlock() { mutex_exit(&mutex); }
};
using PicoUsbBridge=UsbBridge<UsbMutex>;
PicoUsbBridge& usb_bridge();
// Internal worker seams also used by the native production-adapter harness.
bool initialize_usb_worker();
void usb_worker_step();
}
