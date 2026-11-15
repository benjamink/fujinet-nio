#include "fujinet/console/console_engine.h"

namespace fujinet::console {

// No console transport yet (USB CDC is the FujiBus channel); main_pico.cpp skips the console.
std::unique_ptr<IConsoleTransport> create_default_console_transport()
{
    return nullptr;
}

} // namespace fujinet::console
