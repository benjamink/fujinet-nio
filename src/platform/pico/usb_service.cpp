#include "fujinet/platform/pico/usb_service.h"
#include "fujinet/platform/pico/stack_probe.h"
#include "detail/usb_worker.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/sem.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "device/usbd_pvt.h"

namespace fujinet::platform::pico {
namespace {
// Constructed on core 0 before launch; never stores a Channel pointer.
detail::PicoUsbBridge bridge;
semaphore_t startup;
bool startup_ok=false;
struct CdcDevice {
    void clear_rx() { tud_cdc_read_flush(); }
    void clear_tx() { tud_cdc_write_clear(); }
    std::size_t read(void* p,std::size_t n) { return tud_cdc_read(p,n); }
    std::size_t write_available() { return tud_cdc_write_available(); }
    std::size_t write(const void* p,std::size_t n) { return tud_cdc_write(p,n); }
    void flush() { tud_cdc_write_flush(); }
};
void worker_main() {
    startup_ok=detail::initialize_usb_worker();
    sem_release(&startup);
    // A failed worker stays alive (including its lockout IRQ, if initialized).
    for(;;) {
        if(startup_ok) detail::usb_worker_step();
        tight_loop_contents();
    }
}
}
namespace detail {
PicoUsbBridge& usb_bridge() { return bridge; }
bool initialize_usb_worker() {
    // TinyUSB installs/enables the USB IRQ on this core. Register flash lockout
    // first, and publish readiness only after both initializations succeed.
    return flash_safe_execute_core_init() && tusb_init();
}
void usb_worker_step() {
    tud_task(); // No bridge lock: task callbacks below may acquire it.
    bridge.connection(tud_cdc_connected());
    CdcDevice device;
    bridge.pump(device);
}
}
bool start_usb_worker() {
    static bool attempted=false;
    if(attempted) return false;
    attempted=true;
    sem_init(&startup,0,1);
    paint_core1_stack(); // the stack is unused until the core starts: measure how deep it goes
    multicore_launch_core1(worker_main);
    // The SDK ROM launch handshake precedes this bounded application readiness
    // wait. Failure must prevent main from reaching flash mount/write paths.
    return sem_acquire_timeout_ms(&startup,2000) && startup_ok;
}
void usb_connection_event(bool connected) {
    const auto epoch=bridge.epoch();
    bridge.connection(connected);
    if(bridge.epoch()!=epoch) {
        CdcDevice device; bridge.clear_device(device);
    }
}
void usb_reset_event() {
    bridge.reset();
    CdcDevice device; bridge.clear_device(device);
}

}
extern "C" void tud_mount_cb() { fujinet::platform::pico::usb_reset_event(); }
extern "C" void tud_umount_cb() { fujinet::platform::pico::usb_reset_event(); }
extern "C" void tud_cdc_line_state_cb(std::uint8_t instance,bool dtr,bool) {
    if(instance==0) fujinet::platform::pico::usb_connection_event(dtr);
}
// TinyUSB invokes every class driver's reset from configuration_reset while
// dispatching BUS_RESET/UNPLUGGED (and configuration changes) in tud_task order.
// An IRQ-side flag cannot distinguish a reset followed by a fresh connection
// in the same task dispatch. This observer claims no interfaces/endpoints.
namespace {
void observer_init() {}
bool observer_deinit() { return true; }
void observer_reset(std::uint8_t) {
    fujinet::platform::pico::detail::usb_bridge().reset_from_device();
}
std::uint16_t observer_open(std::uint8_t,const tusb_desc_interface_t*,std::uint16_t) { return 0; }
bool observer_control(std::uint8_t,std::uint8_t,const tusb_control_request_t*) { return false; }
bool observer_transfer(std::uint8_t,std::uint8_t,xfer_result_t,std::uint32_t) { return false; }
const usbd_class_driver_t reset_observer={
    "NIO session", observer_init, observer_deinit, observer_reset,
    observer_open, observer_control, observer_transfer, nullptr
};
}
extern "C" const usbd_class_driver_t* usbd_app_driver_get_cb(std::uint8_t* count) {
    *count=1;
    return &reset_observer;
}
