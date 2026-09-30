#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/boot_mount.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

std::unique_ptr<fujinet::tests::MemoryFileSystem> make_host_fs_with_boot()
{
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(fs->createDirectory("/boot"));
    const std::vector<std::uint8_t> bytes{0, 1, 2, 3};
    REQUIRE(fs->create_file("/boot/autorun.atr", bytes));
    return fs;
}

} // namespace

TEST_CASE("apply_boot_mount skips normal mode")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Normal;
    boot.configUri = "persist:/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount applies config disk as pending read-only mount")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    auto pending = disk.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
    CHECK(pending->mode == "r");
    CHECK(pending->enabled == true);
}

TEST_CASE("apply_boot_mount skips missing config disk")
{
    fujinet::fs::StorageManager storage;
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(storage.registerFileSystem(std::move(fs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount stages a network config disk without probing it")
{
    // Nothing exists on the fake network filesystem: at boot the link is not
    // up, so the image can't be probed and must be left to the lazy mount.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::NetworkTnfs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "tnfs://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    auto pending = disk.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "tnfs://server.example/boot/autorun.atr");
}

TEST_CASE("apply_boot_mount recognises a network filesystem regardless of URI spelling")
{
    // The decision follows the filesystem the URI resolves to, so scheme case
    // (resolved case-insensitively) cannot change it.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::NetworkTnfs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "TNFS://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    CHECK(disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount stages an HTTP config disk without probing it")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "http", fujinet::fs::FileSystemKind::NetworkHttp)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "http://server.example/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    CHECK(disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount still probes a local filesystem whatever its name")
{
    // A local filesystem registered under a network-looking name is probed as
    // before: the filesystem's kind decides, not its name or URI scheme.
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>(
        "tnfs", fujinet::fs::FileSystemKind::HostPosix)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "tnfs://server.example/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("is_network_kind classifies every filesystem kind")
{
    using fujinet::fs::FileSystemKind;
    using fujinet::fs::is_network_kind;
    CHECK(is_network_kind(FileSystemKind::NetworkTnfs));
    CHECK(is_network_kind(FileSystemKind::NetworkSmb));
    CHECK(is_network_kind(FileSystemKind::NetworkFtp));
    CHECK(is_network_kind(FileSystemKind::NetworkHttp));
    CHECK_FALSE(is_network_kind(FileSystemKind::LocalFlash));
    CHECK_FALSE(is_network_kind(FileSystemKind::LocalSD));
    CHECK_FALSE(is_network_kind(FileSystemKind::HostPosix));
    CHECK_FALSE(is_network_kind(FileSystemKind::Unknown));
}

TEST_CASE("apply_boot_mount uses the bootstrap-selected active disk unit")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 1) == 1);
    CHECK(!disk.get_pending_mount(0).has_value());
    auto pending = disk.get_pending_mount(1);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
}
