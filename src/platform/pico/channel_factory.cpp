#include "fujinet/platform/channel_factory.h"

#include "fujinet/core/logging.h"

namespace fujinet::platform {

// USB CDC and UART channels are not implemented yet.
std::unique_ptr<io::Channel>
create_channel_for_profile(const build::BuildProfile& /*profile*/, const config::FujiConfig& /*config*/, disk::DiskService*)
{
    FN_LOGE("platform", "No channel implemented for the Pico 2 W yet");
    return nullptr;
}

} // namespace fujinet::platform
