#include "fujinet/platform/channel_factory.h"
#include "fujinet/platform/pico/usb_service.h"
#include "fujinet/build/profile.h"
#include "fujinet/core/logging.h"
#include "detail/usb_tx.h"
#include "tusb.h"
#include <algorithm>
namespace fujinet::platform::pico {
namespace {
class UsbChannel;
UsbChannel* channel = nullptr;
class UsbChannel final : public io::Channel {
    detail::UsbTx<16384> tx_;
public:
    UsbChannel() { channel=this; }
    ~UsbChannel() override { channel=nullptr; }
    void pump() {
        tx_.connection(tud_cdc_connected());
        if(!tx_.ready()) { tud_cdc_read_flush(); tud_cdc_write_clear(); return; }
        std::uint8_t bytes[64];
        // One bounded USB packet per loop iteration, preserves byte ordering.
        const auto n=tx_.take(bytes,std::min<std::size_t>(sizeof(bytes),tud_cdc_write_available()));
        if(n) tud_cdc_write(bytes,n); // FIFO space was checked; same-thread ownership.
        tud_cdc_write_flush();
    }
    bool available() override { return tx_.ready() && tud_cdc_available(); }
    std::size_t read(std::uint8_t* p,std::size_t n) override { return tx_.ready() ? tud_cdc_read(p,n) : 0; }
    void write(const std::uint8_t* p,std::size_t n) override { tx_.write(p,n); }
    std::uint32_t connection_epoch() const override { return tx_.epoch(); }
};
}
void init_usb() { tusb_init(); }
void service_usb() { tud_task(); if(channel) channel->pump(); }
}
namespace fujinet::platform {
std::unique_ptr<io::Channel> create_channel_for_profile(const build::BuildProfile& profile,const config::FujiConfig&,disk::DiskService*) {
    if(profile.primaryChannel == build::ChannelKind::UsbCdcDevice) return std::make_unique<pico::UsbChannel>();
    FN_LOGE("platform","Unsupported Pico channel"); return nullptr;
}
}
