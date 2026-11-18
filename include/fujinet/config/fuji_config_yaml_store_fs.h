#pragma once

#include <memory>
#include <string>

#include "fujinet/config/fuji_config.h"
#include "fujinet/fs/filesystem.h"

namespace fujinet::fs { class StorageManager; }

namespace fujinet::config {

class YamlFujiConfigStoreFs : public FujiConfigStore {
public:
    // primary can be null (no SD), backup can be null (no flash) – we handle both.
    // `defaults` is what a missing file, an empty file or a missing key yields.
    // Only the Visor platform factory requests emission of default Visor settings.
    YamlFujiConfigStoreFs(fs::IFileSystem* primary,
                          fs::IFileSystem* backup,
                          std::string      relativePath,
                          FujiConfig       defaults = {},
                          bool             emitVisorDefaults = false);

    FujiConfig load() override;
    void save(const FujiConfig& cfg) override;

private:
    fs::IFileSystem* _primary;
    fs::IFileSystem* _backup;
    std::string      _relPath; // e.g. "fujinet.yaml"
    FujiConfig       _defaults;
    bool             _emitVisorDefaults; // selected by the platform factory

    FujiConfig loadFromFs(fs::IFileSystem& fs);
    void saveToFs(fs::IFileSystem& fs, const FujiConfig& cfg);
};

// The config store for a device with local storage: the SD card ("sd0") is the primary when one
// was mounted at start-up, with flash as a mirror that every save also writes, so the settings
// (wifi credentials among them) survive a pulled card and seed a new one. With no card, flash is
// the primary. With neither, the store is in memory only. Shared by the ESP32 and the Pico so a
// user gets the same behaviour from either.
std::unique_ptr<FujiConfigStore> make_local_storage_config_store(fs::StorageManager& storage,
                                                                 FujiConfig defaults);

} // namespace fujinet::config
