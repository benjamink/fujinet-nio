#include "tusb.h"
#include "pico/unique_id.h"
#include <cstring>
#include <cstdint>

// Raspberry Pi SDK's development VID/PID. Assign a production PID before release.
static const tusb_desc_device_t device = {
    sizeof(tusb_desc_device_t), TUSB_DESC_DEVICE, 0x0200,
    TUSB_CLASS_MISC, MISC_SUBCLASS_COMMON, MISC_PROTOCOL_IAD,
    CFG_TUD_ENDPOINT0_SIZE, 0x2e8a, 0x000a, 0x0100, 1, 2, 3, 1
};
static const std::uint8_t config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64)
};
extern "C" const std::uint8_t* tud_descriptor_device_cb() { return reinterpret_cast<const std::uint8_t*>(&device); }
extern "C" const std::uint8_t* tud_descriptor_configuration_cb(std::uint8_t) { return config; }
extern "C" const std::uint16_t* tud_descriptor_string_cb(std::uint8_t index, std::uint16_t) {
    static std::uint16_t out[64];
    static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    if(index == 0) { out[0]=(TUSB_DESC_STRING<<8)|4; out[1]=0x0409; return out; }
    pico_get_unique_board_id_string(serial,sizeof(serial));
    const char* strings[]={"", "FujiNet", "FujiNet NIO Pico 2 W", serial, "FujiBus"};
    if(index >= sizeof(strings)/sizeof(*strings)) return nullptr;
    const auto n=std::strlen(strings[index]);
    for(std::size_t i=0;i<n;++i) out[i+1]=strings[index][i];
    out[0]=(TUSB_DESC_STRING<<8)|(2*n+2); return out;
}
