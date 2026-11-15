#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusSlip,
        .primaryChannel   = ChannelKind::UsbCdcDevice,
        .packetLink       = false,
        .name             = "RP2350 Pico 2 W + FujiBus over USB CDC",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
