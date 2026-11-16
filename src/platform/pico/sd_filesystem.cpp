#include "detail/sd_filesystem.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include "ff.h"
#include "diskio.h"

namespace fujinet::platform::pico::detail {
namespace {

SdCard* g_card = nullptr;
ClockFn g_clock = nullptr;

bool is_disk_error(FRESULT r) { return r == FR_DISK_ERR || r == FR_NOT_READY || r == FR_INT_ERR; }

} // namespace
} // namespace fujinet::platform::pico::detail

// FatFs's block device interface, backed by the one SdCard.
extern "C" {

using fujinet::platform::pico::detail::g_card;
using fujinet::platform::pico::detail::g_clock;

DSTATUS disk_status(BYTE) { return g_card && g_card->ready() ? 0 : STA_NOINIT; }

DSTATUS disk_initialize(BYTE)
{
    if (!g_card) return STA_NOINIT;
    if (!g_card->ready() && !g_card->init()) return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE, BYTE* buff, LBA_t sector, UINT count)
{
    return g_card && g_card->read(static_cast<std::uint32_t>(sector), buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE, const BYTE* buff, LBA_t sector, UINT count)
{
    return g_card && g_card->write(static_cast<std::uint32_t>(sector), buff, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE, BYTE cmd, void* buff)
{
    if (!g_card || !g_card->ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return g_card->sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = g_card->sector_count(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = 512; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK; // erase block size unknown
    default: return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    // FAT time: 1980-based years, 2-second resolution. An unset clock stamps 2026-01-01.
    std::uint64_t seconds = g_clock ? g_clock() : 0;
    if (seconds < 1577836800ULL) seconds = 1767225600ULL;
    const std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return (static_cast<DWORD>(tm.tm_year - 80) << 25) | (static_cast<DWORD>(tm.tm_mon + 1) << 21) |
           (static_cast<DWORD>(tm.tm_mday) << 16) | (static_cast<DWORD>(tm.tm_hour) << 11) |
           (static_cast<DWORD>(tm.tm_min) << 5) | static_cast<DWORD>(tm.tm_sec / 2);
}

} // extern "C"

namespace fujinet::platform::pico::detail {
namespace {

namespace fs = fujinet::fs;

struct Volume {
    SdCard& card;
    FATFS fatfs{};
    bool mounted = false;
    std::uint32_t retry_after_ms = 0;

    explicit Volume(SdCard& c) : card(c) {}
    ~Volume() { if (mounted) f_unmount(""); }

    bool mount_now() {
        const FRESULT r = f_mount(&fatfs, "", 1);
        mounted = r == FR_OK;
        return mounted;
    }
    // A card that errored (pulled, glitched) is re-initialised on the next operation; files
    // opened before that fail cleanly, because the remount invalidates them.
    bool ensure() {
        if (mounted && card.ready()) return true;
        // After a failed attempt wait a second before the next: initialising an absent card
        // takes a while and every filesystem call would otherwise pay for it.
        if (!mounted && static_cast<std::int32_t>(card.now_ms() - retry_after_ms) < 0) return false;
        if (mounted) { f_unmount(""); mounted = false; }
        if (mount_now()) return true;
        retry_after_ms = card.now_ms() + 1000;
        return false;
    }
    void note(FRESULT r) { if (is_disk_error(r) && mounted) { f_unmount(""); mounted = false; } }
};

// FatFs paths: no volume prefix, absolute, '/' separated. Reject what FatFs would treat specially.
bool valid_path(const std::string& path)
{
    if (path.empty() || path.size() > 1024 || path.front() != '/' ) return false;
    if (path.find('\0') != std::string::npos) return false;
    std::size_t pos = 0;
    while (pos < path.size()) {
        const auto end = path.find('/', pos);
        const auto part = path.substr(pos, end == std::string::npos ? end : end - pos);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return true;
}

// FatFs wants "/" spelled as an empty name for the root directory.
std::string fat_path(const std::string& path)
{
    std::string out = path;
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::chrono::system_clock::time_point fat_time(WORD date, WORD time)
{
    if (date == 0) return {};
    std::tm tm{};
    tm.tm_year = (date >> 9) + 80; tm.tm_mon = ((date >> 5) & 0x0F) - 1; tm.tm_mday = date & 0x1F;
    tm.tm_hour = time >> 11; tm.tm_min = (time >> 5) & 0x3F; tm.tm_sec = (time & 0x1F) * 2;
    // days from civil, so no dependence on timegm
    const int y = tm.tm_year + 1900 - (tm.tm_mon < 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned mp = static_cast<unsigned>((tm.tm_mon + 1 + 9) % 12) /* civil month 3..14 */;
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(tm.tm_mday) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = static_cast<long>(era) * 146097 + static_cast<long>(doe) - 719468;
    return std::chrono::system_clock::time_point(std::chrono::seconds(days * 86400L + tm.tm_hour * 3600L + tm.tm_min * 60L + tm.tm_sec));
}

class File final : public fs::IFile {
    std::shared_ptr<Volume> volume_;
    std::unique_ptr<FIL> file_ = std::make_unique<FIL>();
    bool opened_ = false;
public:
    explicit File(std::shared_ptr<Volume> v) : volume_(std::move(v)) {}
    ~File() override { if (opened_) f_close(file_.get()); }
    bool open(const std::string& path, BYTE mode) {
        const FRESULT r = f_open(file_.get(), path.c_str(), mode);
        volume_->note(r);
        opened_ = r == FR_OK;
        return opened_;
    }
    std::size_t read(void* p, std::size_t n) override {
        UINT got = 0;
        const FRESULT r = f_read(file_.get(), p, static_cast<UINT>(std::min<std::size_t>(n, 0x7FFFFFFF)), &got);
        volume_->note(r);
        return r == FR_OK ? got : 0;
    }
    std::size_t write(const void* p, std::size_t n) override {
        UINT put = 0;
        const FRESULT r = f_write(file_.get(), p, static_cast<UINT>(std::min<std::size_t>(n, 0x7FFFFFFF)), &put);
        volume_->note(r);
        return r == FR_OK ? put : 0;
    }
    bool seek(std::uint64_t offset) override {
        if (offset > 0xFFFFFFFFull) return false;
        const FRESULT r = f_lseek(file_.get(), static_cast<FSIZE_t>(offset));
        volume_->note(r);
        return r == FR_OK && f_tell(file_.get()) == offset; // f_lseek clamps in a read-only file
    }
    std::uint64_t tell() const override { return f_tell(file_.get()); }
    bool flush() override { const FRESULT r = f_sync(file_.get()); volume_->note(r); return r == FR_OK; }
};

class FileSystem final : public fs::IFileSystem {
    std::shared_ptr<Volume> volume_;

    bool info(const std::string& path, FILINFO& out) {
        if (!valid_path(path) || !volume_->ensure()) return false;
        if (path == "/") { out = {}; out.fattrib = AM_DIR; return true; }
        const FRESULT r = f_stat(fat_path(path).c_str(), &out);
        volume_->note(r);
        return r == FR_OK;
    }
public:
    explicit FileSystem(std::shared_ptr<Volume> v) : volume_(std::move(v)) {}
    fs::FileSystemKind kind() const override { return fs::FileSystemKind::LocalSD; }
    std::string name() const override { return "sd0"; }

    bool exists(const std::string& p) override { auto i = std::make_unique<FILINFO>(); return info(p, *i); }
    bool isDirectory(const std::string& p) override { auto i = std::make_unique<FILINFO>(); return info(p, *i) && (i->fattrib & AM_DIR); }

    bool createDirectory(const std::string& p) override {
        if (!valid_path(p) || p == "/" || !volume_->ensure()) return false;
        const FRESULT r = f_mkdir(fat_path(p).c_str()); volume_->note(r); return r == FR_OK;
    }
    bool removeFile(const std::string& p) override {
        auto i = std::make_unique<FILINFO>();
        if (!info(p, *i) || (i->fattrib & AM_DIR) || p == "/") return false;
        const FRESULT r = f_unlink(fat_path(p).c_str()); volume_->note(r); return r == FR_OK;
    }
    bool removeDirectory(const std::string& p) override {
        auto i = std::make_unique<FILINFO>();
        if (p == "/" || !info(p, *i) || !(i->fattrib & AM_DIR)) return false;
        const FRESULT r = f_unlink(fat_path(p).c_str()); volume_->note(r); return r == FR_OK; // fails if not empty
    }
    bool rename(const std::string& from, const std::string& to) override {
        if (!valid_path(from) || !valid_path(to) || from == "/" || to == "/" || !volume_->ensure()) return false;
        const FRESULT r = f_rename(fat_path(from).c_str(), fat_path(to).c_str()); volume_->note(r); return r == FR_OK;
    }
    std::unique_ptr<fs::IFile> open(const std::string& path, const char* mode) override {
        if (!valid_path(path) || path == "/" || !mode || !*mode || !volume_->ensure()) return nullptr;
        BYTE flags = 0;
        if (mode[0] == 'r') flags = FA_READ;
        else if (mode[0] == 'w') flags = FA_WRITE | FA_CREATE_ALWAYS;
        else if (mode[0] == 'a') flags = FA_WRITE | FA_OPEN_APPEND;
        else return nullptr;
        bool plus = false, binary = false;
        for (const char* c = mode + 1; *c; ++c) {
            if (*c == '+' && !plus) plus = true;
            else if (*c == 'b' && !binary) binary = true;
            else return nullptr;
        }
        if (plus) flags |= FA_READ | FA_WRITE;
        auto file = std::make_unique<File>(volume_);
        if (!file->open(fat_path(path), flags)) return nullptr;
        return file;
    }
    bool stat(const std::string& path, fs::FileInfo& out) override {
        auto i = std::make_unique<FILINFO>();
        if (!info(path, *i)) return false;
        out = {}; out.path = path; out.isDirectory = (i->fattrib & AM_DIR) != 0; out.sizeBytes = out.isDirectory ? 0 : i->fsize;
        out.modifiedTime = fat_time(i->fdate, i->ftime);
        return true;
    }
    bool listDirectory(const std::string& path, std::vector<fs::FileInfo>& out) override {
        out.clear();
        if (!valid_path(path) || !volume_->ensure()) return false;
        DIR dir{};
        FRESULT r = f_opendir(&dir, fat_path(path).c_str());
        volume_->note(r);
        if (r != FR_OK) return false;
        auto item = std::make_unique<FILINFO>();
        for (;;) {
            r = f_readdir(&dir, item.get());
            if (r != FR_OK || item->fname[0] == 0) break;
            fs::FileInfo entry{};
            entry.path = path + (path.back() == '/' ? "" : "/") + item->fname;
            entry.isDirectory = (item->fattrib & AM_DIR) != 0;
            entry.sizeBytes = entry.isDirectory ? 0 : item->fsize;
            entry.modifiedTime = fat_time(item->fdate, item->ftime);
            out.push_back(std::move(entry));
        }
        volume_->note(r);
        f_closedir(&dir);
        return r == FR_OK;
    }
};

} // namespace

std::unique_ptr<fs::IFileSystem> mount_sd_filesystem(SdCard& card, ClockFn clock)
{
    g_card = &card;
    g_clock = clock;
    auto volume = std::make_shared<Volume>(card);
    if (!volume->mount_now()) return nullptr;
    return std::make_unique<FileSystem>(std::move(volume));
}

bool format_sd_card(SdCard& card)
{
    g_card = &card;
    if (!card.ready() && !card.init()) return false;
    std::vector<BYTE> work(FF_MAX_SS * 8); // f_mkfs working area
    const MKFS_PARM opt{FM_ANY, 0, 0, 0, 0}; // FAT32 (or smaller by size) behind an MBR, as cards are sold
    if (f_mkfs("", &opt, work.data(), static_cast<UINT>(work.size())) != FR_OK) return false;
    FATFS fatfs{};
    const bool ok = f_mount(&fatfs, "", 1) == FR_OK; // proves the new filesystem reads back
    f_unmount("");
    return ok;
}

} // namespace fujinet::platform::pico::detail
