#include "fujinet/fs/persist_sync.h"

#include <cstdint>
#include <vector>

namespace fujinet::fs {

namespace {

constexpr const char* kTree = "/FujiNet";
constexpr const char* kConfigFile = "/fujinet.yaml";
constexpr const char* kTempSuffix = ".sync-tmp";
constexpr int kMaxDepth = 8;
// Small, so it is safe in a console or service task on the smallest target.
constexpr std::size_t kChunk = 512;

bool is_local_storage(const IFileSystem& fs)
{
    const auto kind = fs.kind();
    return kind == FileSystemKind::LocalFlash || kind == FileSystemKind::LocalSD;
}

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string leaf(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join(const std::string& dir, const std::string& name)
{
    return (dir.empty() || dir.back() != '/') ? dir + "/" + name : dir + name;
}

class Syncer {
public:
    Syncer(IFileSystem& from, IFileSystem& to, const PersistSyncOptions& options, const SyncObserver& observe)
        : _from(from), _to(to), _options(options), _observe(observe), _buf(2 * kChunk)
    {}

    PersistSyncSummary run()
    {
        FileInfo st{};
        if (_from.stat(kTree, st) && st.isDirectory) {
            walk(kTree, 0);
        }
        if (_from.stat(kConfigFile, st) && !st.isDirectory) {
            file(kConfigFile, st.sizeBytes);
        }
        _summary.ok = _summary.failed == 0;
        return _summary;
    }

private:
    void report(const std::string& path, SyncAction action)
    {
        switch (action) {
        case SyncAction::Copy: ++_summary.copied; break;
        case SyncAction::Overwrite: ++_summary.overwritten; break;
        case SyncAction::Identical: ++_summary.identical; break;
        case SyncAction::Conflict: ++_summary.conflicts; break;
        case SyncAction::Failed: ++_summary.failed; break;
        }
        if (_observe) _observe(path, action);
    }

    // Creates each missing directory along `path` on the destination.
    bool ensure_dir(const std::string& path)
    {
        std::string cur;
        std::size_t i = 0;
        while (i < path.size()) {
            while (i < path.size() && path[i] == '/') ++i;
            if (i >= path.size()) break;
            const auto j = path.find('/', i);
            const auto end = (j == std::string::npos) ? path.size() : j;
            cur += "/" + path.substr(i, end - i);
            if (!_to.isDirectory(cur) && !_to.createDirectory(cur) && !_to.isDirectory(cur)) {
                return false;
            }
            i = end;
        }
        return true;
    }

    void walk(const std::string& dir, int depth)
    {
        if (depth > kMaxDepth) {
            report(dir, SyncAction::Failed);
            return;
        }
        std::vector<FileInfo> entries;
        if (!_from.listDirectory(dir, entries)) {
            report(dir, SyncAction::Failed);
            return;
        }
        for (const auto& e : entries) {
            const std::string name = leaf(e.path);
            if (name.empty() || ends_with(name, kTempSuffix)) continue;  // a leftover of an interrupted sync
            const std::string path = join(dir, name);
            if (e.isDirectory) {
                walk(path, depth + 1);
            } else {
                file(path, e.sizeBytes);
            }
        }
    }

    // True when both files hold the same bytes.
    bool same_content(const std::string& path)
    {
        auto a = _from.open(path, "rb");
        auto b = _to.open(path, "rb");
        if (!a || !b) return false;
        std::uint8_t* const ba = _buf.data();
        std::uint8_t* const bb = _buf.data() + kChunk;
        for (;;) {
            const std::size_t na = a->read(ba, kChunk);
            // Read the same count from the other file, tolerating short reads.
            std::size_t nb = 0;
            while (nb < na) {
                const std::size_t got = b->read(bb + nb, na - nb);
                if (got == 0) break;
                nb += got;
            }
            if (na != nb) return false;
            if (na == 0) {
                // Both at the end only if the other file has nothing more.
                return b->read(bb, 1) == 0;
            }
            for (std::size_t i = 0; i < na; ++i) {
                if (ba[i] != bb[i]) return false;
            }
        }
    }

    bool copy(const std::string& path, bool replace)
    {
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos && slash > 0 && !ensure_dir(path.substr(0, slash))) return false;

        const std::string tmp = path + kTempSuffix;
        if (_to.exists(tmp)) _to.removeFile(tmp);

        bool ok = false;
        {
            auto in = _from.open(path, "rb");
            auto out = _to.open(tmp, "wb");
            if (in && out) {
                ok = true;
                for (;;) {
                    const std::size_t n = in->read(_buf.data(), kChunk);
                    if (n == 0) break;
                    if (out->write(_buf.data(), n) != n) { ok = false; break; }
                }
                ok = ok && out->flush();
            }
        }  // both files closed here, before the rename
        if (ok && replace) ok = _to.removeFile(path);
        if (ok) ok = _to.rename(tmp, path);
        if (!ok && _to.exists(tmp)) _to.removeFile(tmp);
        return ok;
    }

    void file(const std::string& path, std::uint64_t size)
    {
        FileInfo dst{};
        const bool exists = _to.stat(path, dst) && !dst.isDirectory;
        if (exists && dst.sizeBytes == size && same_content(path)) {
            report(path, SyncAction::Identical);
            return;
        }
        if (exists && !_options.overwrite) {
            report(path, SyncAction::Conflict);
            return;
        }
        const SyncAction action = exists ? SyncAction::Overwrite : SyncAction::Copy;
        if (!_options.apply) {
            report(path, action);
            return;
        }
        report(path, copy(path, exists) ? action : SyncAction::Failed);
    }

    IFileSystem& _from;
    IFileSystem& _to;
    PersistSyncOptions _options;
    const SyncObserver& _observe;
    std::vector<std::uint8_t> _buf;
    PersistSyncSummary _summary;
};

} // namespace

PersistSyncSummary sync_persistent(IFileSystem& from,
                                   IFileSystem& to,
                                   const PersistSyncOptions& options,
                                   const SyncObserver& observe)
{
    PersistSyncSummary refused;
    if (&from == &to) {
        refused.error = "source and destination are the same filesystem";
    } else if (!is_local_storage(from) || !is_local_storage(to)) {
        refused.error = "only local flash and SD filesystems can be synced";
    } else if (!from.available()) {
        refused.error = from.name() + " is not available";
    } else if (!to.available()) {
        refused.error = to.name() + " is not available";
    }
    if (!refused.error.empty()) return refused;

    return Syncer(from, to, options, observe).run();
}

} // namespace fujinet::fs
