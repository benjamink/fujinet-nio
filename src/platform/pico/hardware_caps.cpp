#include "fujinet/build/profile.h"

namespace fujinet::build {

// Pico 2 W: CYW43439 Wi-Fi, 520 KB SRAM, 4 MB QSPI flash on the board.
HardwareCapabilities detect_hardware_capabilities()
{
    HardwareCapabilities caps{};
    caps.network.hasLocalNetwork = true;
    caps.network.managesItsOwnLink = true;       // CYW43 is configured on-device
    caps.network.supportsAccessPointMode = true; // the radio can do it; not wired up yet
    caps.memory.persistentStorageBytes = 2u * 1024u * 1024u; // flash filesystem budget (not yet used)
    caps.memory.largeMemoryPoolBytes = 0;
    caps.memory.hasDedicatedLargePool = false;
    caps.hasUsbDevice = true;
    caps.hasUsbHost = false;
    return caps;
}

} // namespace fujinet::build
