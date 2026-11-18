#include "doctest.h"

#include "fujinet/console/console_commands.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/console/console_parse.h"
#include "fujinet/console/fs_shell.h"
#include "fujinet/diag/diagnostic_registry.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/fs/storage_manager.h"

#include "fake_fs.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::tests {
namespace {

class FakeConsoleTransport final : public fujinet::console::IConsoleTransport {
public:
    bool connected{true};
    std::deque<std::uint8_t> in;
    std::string out;

    bool is_connected() const override { return connected; }

    bool read_byte(std::uint8_t& outb, int /*timeout_ms*/) override
    {
        if (in.empty()) return false;
        outb = in.front();
        in.pop_front();
        return true;
    }

    void write(std::string_view s) override { out.append(s.data(), s.size()); }

    void write_line(std::string_view s) override
    {
        out.append(s.data(), s.size());
        out.push_back('\n');
    }

    void push_line(std::string_view s)
    {
        for (char c : s) in.push_back(static_cast<std::uint8_t>(c));
        in.push_back(static_cast<std::uint8_t>('\r'));
    }
};

class DummyDiagProvider final : public fujinet::diag::IDiagnosticProvider {
public:
    explicit DummyDiagProvider(std::string_view id, std::vector<fujinet::diag::DiagCommandSpec> cmds)
        : _id(id), _cmds(std::move(cmds))
    {}

    std::string_view provider_id() const noexcept override { return _id; }

    void list_commands(std::vector<fujinet::diag::DiagCommandSpec>& out) const override
    {
        out.insert(out.end(), _cmds.begin(), _cmds.end());
    }

    fujinet::diag::DiagResult execute(const fujinet::diag::DiagArgsView& args) override
    {
        (void)args;
        return fujinet::diag::DiagResult::not_found("not found");
    }

private:
    std::string _id;
    std::vector<fujinet::diag::DiagCommandSpec> _cmds;
};

static bool contains(std::string_view hay, std::string_view needle)
{
    return hay.find(needle) != std::string_view::npos;
}

static std::vector<std::string_view> sv_argv(const std::vector<std::string>& v)
{
    std::vector<std::string_view> out;
    out.reserve(v.size());
    for (const auto& s : v) out.push_back(s);
    return out;
}

} // namespace

TEST_CASE("console_parse split_ws")
{
    using fujinet::console::split_ws;
    CHECK(split_ws("") == std::vector<std::string_view>{});
    CHECK(split_ws("   ") == std::vector<std::string_view>{});

    auto v = split_ws("  a  b\tc ");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("ConsoleCommandRegistry register/dispatch")
{
    using fujinet::console::ConsoleCommandRegistry;
    using fujinet::console::ConsoleCommandSpec;

    ConsoleCommandRegistry reg;
    int count = 0;

    CHECK(reg.register_command(ConsoleCommandSpec{"a", "A", "a"}, [&](const auto&) {
        ++count;
        return true;
    }));
    CHECK(!reg.register_command(ConsoleCommandSpec{"a", "A2", "a"}, [&](const auto&) {
        return true;
    }));

    std::vector<std::string> argv_s = {"a"};
    auto r = reg.dispatch(sv_argv(argv_s));
    REQUIRE(r.has_value());
    CHECK(*r == true);
    CHECK(count == 1);
}

TEST_CASE("ConsoleEngine help prints diagnostic sections")
{
    fujinet::diag::DiagnosticRegistry dreg;

    DummyDiagProvider corep(
        "core",
        {
            fujinet::diag::DiagCommandSpec{.name = "core.stats", .summary = "stats", .usage = "core.stats"},
        });
    DummyDiagProvider diskp(
        "disk",
        {
            fujinet::diag::DiagCommandSpec{.name = "disk.slots", .summary = "slots", .usage = "disk.slots"},
        });
    dreg.add_provider(corep);
    dreg.add_provider(diskp);

    FakeConsoleTransport io;
    fujinet::console::ConsoleEngine eng(dreg, io);

    // First step should emit MOTD + prompt on initial connect.
    CHECK(eng.step(0));
    CHECK(contains(io.out, "fujinet-nio diagnostic console"));
    CHECK(contains(io.out, "> "));

    // Then run help.
    io.push_line("help");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "commands:"));
    CHECK(contains(io.out, "diagnostics:"));
    CHECK(contains(io.out, "[core]"));
    CHECK(contains(io.out, "core.stats"));
    CHECK(contains(io.out, "[disk]"));
    CHECK(contains(io.out, "disk.slots"));
}

TEST_CASE("ConsoleEngine format needs a handler and an explicit confirm")
{
    fujinet::diag::DiagnosticRegistry dreg;
    FakeConsoleTransport io;
    fujinet::console::ConsoleEngine eng(dreg, io);
    CHECK(eng.step(0)); // MOTD + prompt

    // No platform hook: refused, nothing to call.
    io.push_line("format flash confirm");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "format not supported"));

    std::vector<std::string> formatted;
    int reboots = 0;
    bool format_ok = true;
    eng.set_format_handler([&](std::string_view name) {
        formatted.emplace_back(name);
        return format_ok;
    });
    eng.set_reboot_handler([&] { ++reboots; });

    // Without "confirm" (or with extra words) the handler is never called.
    for (const char* line : {"format", "format flash", "format flash now", "format flash confirm extra"}) {
        io.out.clear();
        io.push_line(line);
        CHECK(eng.step(0));
        CHECK(contains(io.out, "usage: format <filesystem> confirm"));
    }
    CHECK(formatted.empty());
    CHECK(reboots == 0);

    // A failing format reports it and does not reboot into a half-made filesystem.
    format_ok = false;
    io.out.clear();
    io.push_line("format flash confirm");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "error: format flash failed"));
    CHECK(formatted == std::vector<std::string>{"flash"});
    CHECK(reboots == 0);

    // Success formats the named filesystem, then reboots.
    format_ok = true;
    io.out.clear();
    io.push_line("format flash confirm");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "format: flash recreated empty"));
    CHECK(formatted == std::vector<std::string>{"flash", "flash"});
    CHECK(reboots == 1);

    // The command is listed in help.
    io.out.clear();
    io.push_line("help");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "format"));
}

TEST_CASE("FsShell fs marks a filesystem whose medium is out, and stops once it is back")
{
    fujinet::fs::StorageManager storage;
    auto card_up = std::make_unique<fujinet::tests::MemoryFileSystem>("sd0");
    auto* card = card_up.get();
    REQUIRE(storage.registerFileSystem(std::move(card_up)));
    REQUIRE(storage.registerFileSystem(std::make_unique<fujinet::tests::MemoryFileSystem>("flash")));

    FakeConsoleTransport io;
    fujinet::console::ConsoleCommandRegistry reg;
    fujinet::console::FsShell sh(storage);
    REQUIRE(sh.register_commands(reg, io));
    std::vector<std::string> argv_s = {"fs"};

    REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
    CHECK(io.out == "flash\nsd0\n");                       // unchanged output while everything works

    io.out.clear(); card->set_available(false);
    REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
    CHECK(io.out == "flash\nsd0 (not available)\n");     // still listed: it recovers by itself

    io.out.clear(); card->set_available(true);
    REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
    CHECK(io.out == "flash\nsd0\n");
}

TEST_CASE("FsShell sync is a dry run by default, copies with --apply, and keeps differing files")
{
    using fujinet::fs::FileSystemKind;
    fujinet::fs::StorageManager storage;
    auto flash_up = std::make_unique<fujinet::tests::MemoryFileSystem>("flash", FileSystemKind::LocalFlash);
    auto sd_up = std::make_unique<fujinet::tests::MemoryFileSystem>("sd0", FileSystemKind::LocalSD);
    auto* flash = flash_up.get();
    auto* sd = sd_up.get();
    REQUIRE(storage.registerFileSystem(std::move(flash_up)));
    REQUIRE(storage.registerFileSystem(std::move(sd_up)));
    REQUIRE(flash->createDirectory("/FujiNet"));
    REQUIRE(flash->create_file("/FujiNet/00010100.key", {1, 2, 3}));
    REQUIRE(flash->create_file("/fujinet.yaml", {'a'}));
    REQUIRE(sd->createDirectory("/FujiNet"));
    REQUIRE(sd->create_file("/fujinet.yaml", {'b'}));   // differs from flash

    FakeConsoleTransport io;
    fujinet::console::ConsoleCommandRegistry reg;
    fujinet::console::FsShell sh(storage);
    REQUIRE(sh.register_commands(reg, io));

    SUBCASE("without --apply nothing changes") {
        std::vector<std::string> argv_s = {"sync", "flash", "sd0"};
        REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
        CHECK(io.out.find("would copy: /FujiNet/00010100.key") != std::string::npos);
        CHECK(io.out.find("differs, kept: /fujinet.yaml") != std::string::npos);
        CHECK(io.out.find("dry run, nothing changed: flash -> sd0: 1 new, 0 overwritten, 0 identical, 1 kept, 0 failed")
              != std::string::npos);
        CHECK(io.out.find("add --apply") != std::string::npos);
        CHECK_FALSE(sd->exists("/FujiNet/00010100.key"));
    }
    SUBCASE("--apply copies the new file and keeps the differing one") {
        std::vector<std::string> argv_s = {"sync", "flash:", "sd0:/", "--apply"};
        REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
        CHECK(io.out.find("copied: /FujiNet/00010100.key") != std::string::npos);
        CHECK(io.out.find("synced flash -> sd0: 1 new, 0 overwritten, 0 identical, 1 kept, 0 failed") != std::string::npos);
        CHECK(io.out.find("use --overwrite") != std::string::npos);
        CHECK(sd->file_bytes("/FujiNet/00010100.key") == std::vector<std::uint8_t>{1, 2, 3});
        CHECK(sd->file_bytes("/fujinet.yaml") == std::vector<std::uint8_t>{'b'});
    }
    SUBCASE("--apply --overwrite replaces it") {
        std::vector<std::string> argv_s = {"sync", "flash", "sd0", "--overwrite", "--apply"};
        REQUIRE(reg.dispatch(sv_argv(argv_s)).has_value());
        CHECK(io.out.find("overwritten: /fujinet.yaml") != std::string::npos);
        CHECK(sd->file_bytes("/fujinet.yaml") == std::vector<std::uint8_t>{'a'});
    }
    SUBCASE("bad arguments and a missing card say so") {
        std::vector<std::string> one = {"sync", "flash"};
        REQUIRE(reg.dispatch(sv_argv(one)).has_value());
        CHECK(io.out.find("usage: sync <from> <to>") != std::string::npos);

        io.out.clear();
        std::vector<std::string> nofs = {"sync", "flash", "nope"};
        REQUIRE(reg.dispatch(sv_argv(nofs)).has_value());
        CHECK(io.out == "error: unknown filesystem: nope\n");

        io.out.clear(); sd->set_available(false);
        std::vector<std::string> out = {"sync", "flash", "sd0", "--apply"};
        REQUIRE(reg.dispatch(sv_argv(out)).has_value());
        CHECK(io.out == "error: sd0 is not available\n");
    }
}

TEST_CASE("FsShell rm/rmdir semantics + wildcards + mv to directory")
{
    fujinet::fs::StorageManager storage;
    auto fs_up = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* fs = fs_up.get();
    REQUIRE(storage.registerFileSystem(std::move(fs_up)));

    REQUIRE(fs->createDirectory("/a"));
    REQUIRE(fs->createDirectory("/b"));
    REQUIRE(fs->create_file("/a/f.txt", {1, 2, 3}));

    FakeConsoleTransport io;
    fujinet::console::ConsoleCommandRegistry reg;
    fujinet::console::FsShell sh(storage);
    sh.cwd_fs() = "mem";
    sh.cwd_path() = "/a";
    REQUIRE(sh.register_commands(reg, io));

    SUBCASE("rmdir on a file fails and does not delete the file")
    {
        std::vector<std::string> argv_s = {"rmdir", "f.txt"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(fs->exists("/a/f.txt"));
        CHECK(contains(io.out, "error: not a directory"));
    }

    SUBCASE("rm wildcard deletes matching files")
    {
        std::vector<std::string> argv_s = {"rm", "*.txt"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a/f.txt"));
    }

    SUBCASE("rm without -r refuses to delete a directory")
    {
        std::vector<std::string> argv_s = {"rm", "/a"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(fs->exists("/a"));
        CHECK(contains(io.out, "error: is a directory"));
    }

    SUBCASE("rmdir -rf deletes a directory tree")
    {
        REQUIRE(fs->createDirectory("/a/sub"));
        REQUIRE(fs->create_file("/a/sub/x.bin", {9}));

        std::vector<std::string> argv_s = {"rmdir", "-rf", "/a"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a"));
        CHECK(!fs->exists("/a/sub/x.bin"));
    }

    SUBCASE("mv destination directory appends basename")
    {
        REQUIRE(fs->create_file("/a/x.bin", {7, 7}));

        std::vector<std::string> argv_s = {"mv", "x.bin", "/b/"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a/x.bin"));
        CHECK(fs->exists("/b/x.bin"));
    }

    SUBCASE("hexdump dumps a file like hexdump -C")
    {
        REQUIRE(fs->create_file(
            "/a/logging.bin",
            {0x00, 0x01, 0x20, 0x41, 0x7e, 0x7f, 0x0a, 0x78,
             0x80, 0x39, 0xff, 0x10, 0x11, 0x12, 0x13, 0x14,
             0x5a}));

        std::vector<std::string> argv_s = {"hexdump", "-n", "17", "logging.bin"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(contains(io.out, "00000000  00 01 20 41 7e 7f 0a 78  80 39 ff 10 11 12 13 14"));
        CHECK(contains(io.out, "|.. A~..x.9......|"));
        CHECK(contains(io.out, "00000010  5a"));
        CHECK(contains(io.out, "|Z|"));
        CHECK(contains(io.out, "00000011"));
    }

    SUBCASE("hexdump defaults to 64 bytes")
    {
        std::vector<std::uint8_t> bytes(80);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<std::uint8_t>(i);
        }
        REQUIRE(fs->create_file("/a/big.bin", bytes));

        std::vector<std::string> argv_s = {"hexdump", "big.bin"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(contains(io.out, "00000030"));
        CHECK(contains(io.out, "00000040"));
        CHECK(!contains(io.out, "00000050"));
    }

    SUBCASE("hexdump rejects directories")
    {
        std::vector<std::string> argv_s = {"hexdump", "/a"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(contains(io.out, "error: is a directory"));
    }
}

} // namespace fujinet::tests

