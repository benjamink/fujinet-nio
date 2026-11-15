#include "fujinet/platform/fuji_config_store_factory.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::platform {

static constexpr const char* TAG = "platform";

// Config lives in fujinet.yaml on the "flash" filesystem; in memory until one is registered.
std::unique_ptr<config::FujiConfigStore> create_fuji_config_store(fs::StorageManager& storage)
{
    const auto defaults = config::default_config_for(build::current_build_profile());
    auto* flash = storage.get("flash");
    if (!flash) {
        FN_LOGW(TAG, "No 'flash' filesystem registered; config stays in memory");
    }
    return std::make_unique<config::YamlFujiConfigStoreFs>(flash, nullptr, "fujinet.yaml", defaults, false);
}

} // namespace fujinet::platform
