// src/platform/esp32/fuji_config_store_factory.cpp

#include "fujinet/platform/fuji_config_store_factory.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/build/profile.h"
#include "fujinet/config/fuji_config_defaults.h"
#include "fujinet/config/fuji_config_yaml_store_fs.h"
#include "fujinet/core/logging.h"

namespace fujinet::platform {


std::unique_ptr<config::FujiConfigStore>
create_fuji_config_store(fs::StorageManager& storage)
{
    // What a fresh device starts from: the profile knows its platform's boot disk.
    const auto defaults = config::default_config_for(build::current_build_profile());

    return config::make_local_storage_config_store(storage, defaults);
}

} // namespace fujinet::platform
