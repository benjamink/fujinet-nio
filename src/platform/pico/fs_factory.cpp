#include "fujinet/platform/pico/fs_factory.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/http_filesystem.h"
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/platform/network_registry.h"

namespace fujinet::platform::pico {

std::unique_ptr<fs::IFileSystem> create_flash_filesystem()
{
    return nullptr; // README, "Filesystem": LittleFS on the on-board flash.
}

std::unique_ptr<fs::IFileSystem> create_tnfs_filesystem(bool /*useTcp*/)
{
    fs::TnfsClientFactory factory = [](const fs::TnfsEndpoint&) -> std::unique_ptr<tnfs::ITnfsClient> {
        return nullptr; // needs the lwIP socket ops
    };
    return fs::make_tnfs_filesystem(std::move(factory));
}

std::unique_ptr<fs::IFileSystem> create_http_filesystem()
{
    auto registry = std::make_shared<io::ProtocolRegistry>(make_default_network_registry());
    fs::HttpProtocolFactory factory = [registry](std::string_view scheme) { return registry->create(scheme); };
    return fs::make_http_filesystem(std::move(factory));
}

} // namespace fujinet::platform::pico
