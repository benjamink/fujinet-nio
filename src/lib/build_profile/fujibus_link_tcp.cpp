#include "fujinet/build/profile.h"

namespace fujinet::build {

BuildProfile current_build_profile()
{
    BuildProfile profile{
        .machine          = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusNative,
        .primaryChannel   = ChannelKind::TcpSocket,
        .packetLink       = true,
        .name             = "POSIX + FujiBus over a packet link on TCP",
        .hw               = {},
    };
    profile.hw = detect_hardware_capabilities();
    return profile;
}

} // namespace fujinet::build
