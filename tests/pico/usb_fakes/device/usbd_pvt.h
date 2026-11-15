#pragma once
#include <cstdint>
struct tusb_desc_interface_t {};
struct tusb_control_request_t {};
enum xfer_result_t { XFER_RESULT_SUCCESS };
struct usbd_class_driver_t {
    const char* name;
    void (*init)();
    bool (*deinit)();
    void (*reset)(std::uint8_t);
    std::uint16_t (*open)(std::uint8_t,const tusb_desc_interface_t*,std::uint16_t);
    bool (*control_xfer_cb)(std::uint8_t,std::uint8_t,const tusb_control_request_t*);
    bool (*xfer_cb)(std::uint8_t,std::uint8_t,xfer_result_t,std::uint32_t);
    void (*sof)(std::uint8_t,std::uint32_t);
};
extern "C" const usbd_class_driver_t* usbd_app_driver_get_cb(std::uint8_t*);
