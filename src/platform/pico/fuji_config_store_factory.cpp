#include "fujinet/platform/fuji_config_store_factory.h"

#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"
#include "fujinet/fs/storage_manager.h"

namespace fujinet::platform {

// Config lives in fujinet.yaml: on the SD card when one was mounted at start-up, mirrored to
// "flash"; on flash alone otherwise. The same as the ESP32 (config::make_local_storage_config_store).
std::unique_ptr<config::FujiConfigStore> create_fuji_config_store(fs::StorageManager& storage)
{
    const auto defaults = config::default_config_for(build::current_build_profile());
    return config::make_local_storage_config_store(storage, defaults);
}

} // namespace fujinet::platform
