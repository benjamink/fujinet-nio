#include "fujinet/platform/channel_factory.h"
#include "fujinet/build/profile.h"
#include "fujinet/core/logging.h"
#include "detail/usb_worker.h"
namespace fujinet::platform::pico {
namespace {
class UsbChannel final : public io::Channel {
    bool attached_=false, has_read_=false;
    std::uint32_t read_epoch_=0;
public:
    bool attach() { return attached_=detail::usb_bridge().attach(); }
    ~UsbChannel() override { if(attached_) detail::usb_bridge().detach(); }
    bool available() override { return detail::usb_bridge().available(); }
    std::size_t read(std::uint8_t* p,std::size_t n) override {
        const auto count=detail::usb_bridge().read(p,n,&read_epoch_);
        if(count) has_read_=true;
        return count;
    }
    void write(const std::uint8_t* p,std::size_t n) override {
        // A slow core-0 request may outlive a disconnect/reconnect on core 1.
        // Only an actual read adopts the new session, never an epoch query.
        if(has_read_) detail::usb_bridge().write_if_epoch(p,n,read_epoch_);
    }
    std::uint32_t connection_epoch() const override { return detail::usb_bridge().epoch(); }
};
}
}
namespace fujinet::platform {
std::unique_ptr<io::Channel> create_channel_for_profile(const build::BuildProfile& profile,const config::FujiConfig&,disk::DiskService*) {
    if(profile.primaryChannel == build::ChannelKind::UsbCdcDevice) {
        // Allocate before attaching: an allocation exception cannot leave a lease.
        auto channel=std::make_unique<pico::UsbChannel>();
        if(channel->attach()) return channel;
        // A rejected second channel must not detach the active one.
        return nullptr;
    }
    FN_LOGE("platform","Unsupported Pico channel"); return nullptr;
}
}
