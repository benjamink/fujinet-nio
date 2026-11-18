#include "doctest.h"

#include "fake_fs.h"
#include "fujinet/fs/persist_sync.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

using fujinet::fs::FileSystemKind;
using fujinet::fs::PersistSyncOptions;
using fujinet::fs::SyncAction;
using fujinet::tests::MemoryFileSystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytes_of(const std::string& s) { return Bytes(s.begin(), s.end()); }

MemoryFileSystem make_flash() { return MemoryFileSystem("flash", FileSystemKind::LocalFlash); }
MemoryFileSystem make_sd() { return MemoryFileSystem("sd0", FileSystemKind::LocalSD); }

// The data a user would not want to set up again, plus something that must never be copied.
void populate_flash(MemoryFileSystem& fs)
{
    fs.createDirectory("/FujiNet");
    fs.createDirectory("/FujiNet/app-store");
    fs.createDirectory("/FujiNet/app-store/v1");
    fs.createDirectory("/FujiNet/app-store/v1/bwc");
    fs.create_file("/FujiNet/00010100.key", bytes_of("lobby-key"));
    fs.create_file("/FujiNet/app-store/v1/bwc/name", bytes_of("mark"));
    fs.create_file("/fujinet.yaml", bytes_of("wifi: home\n"));
    fs.createDirectory("/boot");
    fs.create_file("/boot/autorun.img", bytes_of("disk image, not persistent data"));
}

// What each file in the sync ended up as, keyed by path.
std::map<std::string, SyncAction> run(MemoryFileSystem& from, MemoryFileSystem& to, PersistSyncOptions opt,
                                      fujinet::fs::PersistSyncSummary* summary = nullptr)
{
    std::map<std::string, SyncAction> seen;
    auto s = fujinet::fs::sync_persistent(from, to, opt,
        [&](const std::string& path, SyncAction a) { seen[path] = a; });
    if (summary) *summary = s;
    return seen;
}

} // namespace

TEST_CASE("sync_persistent copies the persistent data to an empty destination")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);

    fujinet::fs::PersistSyncSummary s;
    const auto seen = run(flash, sd, {.apply = true}, &s);

    CHECK(s.ok);
    CHECK(s.copied == 3);
    CHECK(s.failed == 0);
    CHECK(seen.at("/FujiNet/00010100.key") == SyncAction::Copy);
    CHECK(seen.at("/FujiNet/app-store/v1/bwc/name") == SyncAction::Copy);
    CHECK(seen.at("/fujinet.yaml") == SyncAction::Copy);
    CHECK(sd.file_bytes("/FujiNet/00010100.key") == bytes_of("lobby-key"));
    CHECK(sd.file_bytes("/FujiNet/app-store/v1/bwc/name") == bytes_of("mark"));
    CHECK(sd.file_bytes("/fujinet.yaml") == bytes_of("wifi: home\n"));
    // Other data stays where it is, and no temporary file is left behind.
    CHECK_FALSE(sd.exists("/boot/autorun.img"));
    CHECK_FALSE(sd.exists("/FujiNet/00010100.key.sync-tmp"));
    CHECK_FALSE(sd.exists("/fujinet.yaml.sync-tmp"));
}

TEST_CASE("sync_persistent without apply reports what it would do and changes nothing")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);

    fujinet::fs::PersistSyncSummary s;
    const auto seen = run(flash, sd, {}, &s);

    CHECK(s.ok);
    CHECK(s.copied == 3);
    CHECK(seen.size() == 3);
    CHECK_FALSE(sd.exists("/FujiNet"));
    CHECK_FALSE(sd.exists("/fujinet.yaml"));
}

TEST_CASE("sync_persistent is idempotent: a second run finds everything identical")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    run(flash, sd, {.apply = true});

    fujinet::fs::PersistSyncSummary s;
    run(flash, sd, {.apply = true}, &s);

    CHECK(s.ok);
    CHECK(s.identical == 3);
    CHECK(s.copied == 0);
    CHECK(s.conflicts == 0);
}

TEST_CASE("sync_persistent leaves a differing destination file alone unless overwrite is asked for")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    sd.createDirectory("/FujiNet");
    sd.create_file("/FujiNet/00010100.key", bytes_of("newer-key!"));

    fujinet::fs::PersistSyncSummary s;
    auto seen = run(flash, sd, {.apply = true}, &s);
    CHECK(seen.at("/FujiNet/00010100.key") == SyncAction::Conflict);
    CHECK(s.conflicts == 1);
    CHECK(s.copied == 2);
    CHECK(sd.file_bytes("/FujiNet/00010100.key") == bytes_of("newer-key!"));

    seen = run(flash, sd, {.apply = true, .overwrite = true}, &s);
    CHECK(seen.at("/FujiNet/00010100.key") == SyncAction::Overwrite);
    CHECK(s.overwritten == 1);
    CHECK(sd.file_bytes("/FujiNet/00010100.key") == bytes_of("lobby-key"));
    CHECK_FALSE(sd.exists("/FujiNet/00010100.key.sync-tmp"));
}

TEST_CASE("sync_persistent overwrites on a filesystem whose rename refuses an existing file")
{
    // FatFs (the Pico's SD driver) does not replace on rename, so the old file must go first.
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    sd.createDirectory("/FujiNet");
    sd.create_file("/FujiNet/00010100.key", bytes_of("older-key"));
    sd.set_rename_refuses_existing(true);

    fujinet::fs::PersistSyncSummary s;
    run(flash, sd, {.apply = true, .overwrite = true}, &s);

    CHECK(s.ok);
    CHECK(s.overwritten == 1);
    CHECK(sd.file_bytes("/FujiNet/00010100.key") == bytes_of("lobby-key"));
}

TEST_CASE("sync_persistent detects a difference in the last byte of a multi-chunk file")
{
    auto flash = make_flash();
    auto sd = make_sd();
    flash.createDirectory("/FujiNet");
    sd.createDirectory("/FujiNet");
    Bytes big(2000, 0x5A);
    flash.create_file("/FujiNet/big", big);
    big.back() = 0x5B;
    sd.create_file("/FujiNet/big", big);

    fujinet::fs::PersistSyncSummary s;
    run(flash, sd, {.apply = true}, &s);
    CHECK(s.conflicts == 1);
    CHECK(s.identical == 0);

    run(flash, sd, {.apply = true, .overwrite = true}, &s);
    CHECK(sd.file_bytes("/FujiNet/big") == Bytes(2000, 0x5A));
}

TEST_CASE("sync_persistent never deletes or merges: files only in the destination survive")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    sd.createDirectory("/FujiNet");
    sd.create_file("/FujiNet/00010200.key", bytes_of("only-on-card"));

    run(flash, sd, {.apply = true, .overwrite = true});

    CHECK(sd.file_bytes("/FujiNet/00010200.key") == bytes_of("only-on-card"));
}

TEST_CASE("sync_persistent skips a temporary file left over from an interrupted sync")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    flash.create_file("/FujiNet/00010100.key.sync-tmp", bytes_of("half"));

    const auto seen = run(flash, sd, {.apply = true});

    CHECK(seen.count("/FujiNet/00010100.key.sync-tmp") == 0);
    CHECK_FALSE(sd.exists("/FujiNet/00010100.key.sync-tmp"));
}

TEST_CASE("sync_persistent keeps the old destination file when the write fails")
{
    auto flash = make_flash();
    auto sd = make_sd();
    populate_flash(flash);
    sd.createDirectory("/FujiNet");
    sd.create_file("/FujiNet/00010100.key", bytes_of("older-key"));
    sd.set_fail_flush(true);

    fujinet::fs::PersistSyncSummary s;
    const auto seen = run(flash, sd, {.apply = true, .overwrite = true}, &s);

    CHECK_FALSE(s.ok);
    CHECK(s.failed >= 1);
    CHECK(seen.at("/FujiNet/00010100.key") == SyncAction::Failed);
    CHECK(sd.file_bytes("/FujiNet/00010100.key") == bytes_of("older-key"));
    CHECK_FALSE(sd.exists("/FujiNet/00010100.key.sync-tmp"));
}

TEST_CASE("sync_persistent refuses what it cannot do safely")
{
    auto flash = make_flash();
    auto sd = make_sd();
    MemoryFileSystem tnfs("tnfs", FileSystemKind::NetworkTnfs);
    populate_flash(flash);

    SUBCASE("the same filesystem") {
        const auto s = fujinet::fs::sync_persistent(flash, flash, {.apply = true});
        CHECK_FALSE(s.ok);
        CHECK(s.error.find("same") != std::string::npos);
    }
    SUBCASE("a network filesystem") {
        const auto s = fujinet::fs::sync_persistent(flash, tnfs, {.apply = true});
        CHECK_FALSE(s.ok);
        CHECK_FALSE(s.error.empty());
    }
    SUBCASE("a card that is out") {
        sd.set_available(false);
        const auto s = fujinet::fs::sync_persistent(flash, sd, {.apply = true});
        CHECK_FALSE(s.ok);
        CHECK(s.error.find("sd0") != std::string::npos);
        CHECK_FALSE(sd.exists("/FujiNet"));
    }
    SUBCASE("a source that is out") {
        sd.set_available(false);
        const auto s = fujinet::fs::sync_persistent(sd, flash, {.apply = true});
        CHECK_FALSE(s.ok);
        CHECK(s.error.find("sd0") != std::string::npos);
    }
}

TEST_CASE("sync_persistent with nothing to sync succeeds and copies nothing")
{
    auto flash = make_flash();
    auto sd = make_sd();

    fujinet::fs::PersistSyncSummary s;
    run(flash, sd, {.apply = true}, &s);

    CHECK(s.ok);
    CHECK(s.copied == 0);
}
