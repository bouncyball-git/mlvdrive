#include "app/mounter.h"

#include "fs/winfsp_fs.h"
#include "raw/rawfix.h"
#include "vfs/tree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <map>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace app {

namespace {

std::wstring EventSuffix(std::wstring mountPoint) {
    for (auto& ch : mountPoint) {
        if (ch == L':' || ch == L'\\' || ch == L'/') ch = L'_';
        else ch = wchar_t(std::towupper(wint_t(ch)));
    }
    return mountPoint;
}

vfs::BuildOptions BuildOptionsOf(const Settings& s) {
    vfs::BuildOptions options;
    options.dng.output = s.format;
    options.dng.timecode = s.timecode;
    options.fix = s.rawFixes;
    options.fixes.focusPixels = s.focusPixels;
    options.fixes.badPixels = s.badPixels == 3 ? 3 : s.badPixels ? 1 : 0;  // aggressive: auto, searched harder
    options.fixes.badPixelSearch = s.badPixels == 2 ? 1 : 0;
    options.testPatterns = s.testPatterns;
    return options;
}

}  // namespace

std::wstring stopEventName(std::wstring mountPoint) {
    return L"Local\\MLVDrive-stop-" + EventSuffix(std::move(mountPoint));
}

std::wstring rescanEventName(std::wstring mountPoint) {
    return L"Local\\MLVDrive-rescan-" + EventSuffix(std::move(mountPoint));
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// Whether Windows may not report a folder's changes: on a network drive or a share (SMB
// notifications are unreliable: some servers send none, they can stop after a reconnect).
bool IsRemote(const std::wstring& folder) {
    if (folder.rfind(L"\\\\", 0) == 0) return true;  // \\server\share
    wchar_t volume[MAX_PATH];
    if (!GetVolumePathNameW(folder.c_str(), volume, MAX_PATH)) return false;
    return GetDriveTypeW(volume) == DRIVE_REMOTE;
}

constexpr DWORD kPollMs = 30000;

// --- Watching the folders ---
//
// One thread per folder waits for Windows' change notifications (the whole subtree: names, sizes,
// dates); a folder that isn't there (an unplugged drive) is tried again every 5 seconds and counts
// as a change when it comes. Another thread rescans once nothing has changed for 2 seconds, or at
// once when `mlvdrive rescan` sets the named event, and every 30 seconds while some folder may
// change unreported (remote, or Windows won't watch it).
class Mounter::Watcher {
public:
    Watcher(Mounter& owner, const std::vector<std::wstring>& folders, bool watchFolders, const std::wstring& mountPoint)
        : owner_(owner) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        changed_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        rescanNow_ = CreateEventW(nullptr, FALSE, FALSE, rescanEventName(mountPoint).c_str());
        if (watchFolders) {
            for (const auto& f : folders) {
                if (IsRemote(f)) polling_ = true;
                threads_.emplace_back([this, f] { watchFolder(f); });
            }
        }
        threads_.emplace_back([this] { rescanLoop(); });
    }
    bool polling() const { return polling_; }
    ~Watcher() {
        SetEvent(stop_);
        for (auto& t : threads_) t.join();
        CloseHandle(stop_);
        CloseHandle(changed_);
        if (rescanNow_) CloseHandle(rescanNow_);
    }

private:
    bool stopping() const { return WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0; }

    void watchFolder(const std::wstring& folder) {
        bool wasMissing = false;
        while (!stopping()) {
            HANDLE dir = CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (dir == INVALID_HANDLE_VALUE) {
                wasMissing = true;
                WaitForSingleObject(stop_, 5000);
                continue;
            }
            if (wasMissing) SetEvent(changed_);  // the folder (back) again: its clips too
            wasMissing = false;
            if (IsRemote(folder)) polling_ = true;  // (a network drive reconnected)
            alignas(DWORD) uint8_t buffer[16 * 1024];
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            for (;;) {
                ResetEvent(ov.hEvent);
                const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
                if (!ReadDirectoryChangesW(dir, buffer, sizeof buffer, TRUE, filter, nullptr, &ov, nullptr)) {
                    // Windows won't watch it (some network or virtual file systems): looked at
                    // every 30 seconds instead.
                    polling_ = true;
                    CloseHandle(ov.hEvent);
                    CloseHandle(dir);
                    WaitForSingleObject(stop_, INFINITE);
                    return;
                }
                HANDLE wait[2] = {stop_, ov.hEvent};
                if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0) {
                    CancelIoEx(dir, &ov);
                    DWORD n;
                    GetOverlappedResult(dir, &ov, &n, TRUE);
                    break;
                }
                DWORD n = 0;
                if (!GetOverlappedResult(dir, &ov, &n, FALSE)) {  // the folder went away
                    SetEvent(changed_);
                    wasMissing = true;
                    break;
                }
                SetEvent(changed_);  // n == 0: too many changes to list; a change all the same
            }
            CloseHandle(ov.hEvent);
            CloseHandle(dir);
            if (wasMissing) WaitForSingleObject(stop_, 5000);
        }
    }

    void rescanLoop() {
        HANDLE wait[3] = {stop_, changed_, rescanNow_};
        const DWORD count = rescanNow_ ? 3 : 2;
        for (;;) {
            const DWORD w = WaitForMultipleObjects(count, wait, FALSE, kPollMs);
            if (w == WAIT_OBJECT_0) return;
            if (w == WAIT_TIMEOUT && !polling_) continue;  // only folders that need it are polled
            if (w == WAIT_OBJECT_0 + 1) {
                // Until the folders have been quiet for 2 seconds (a copy in progress keeps them busy).
                for (;;) {
                    const DWORD q = WaitForMultipleObjects(2, wait, FALSE, 2000);
                    if (q == WAIT_OBJECT_0) return;
                    if (q == WAIT_TIMEOUT) break;
                }
            }
            RescanResult r;
            std::string error;
            if (owner_.rescan(&r, &error)) {
                r.automatic = true;
                if (owner_.onRescan_) owner_.onRescan_(r);
            }
        }
    }

    Mounter& owner_;
    HANDLE stop_ = nullptr, changed_ = nullptr, rescanNow_ = nullptr;
    std::atomic<bool> polling_{false};
    std::vector<std::thread> threads_;
};

// --- Mounter ---

Mounter::Mounter() = default;

Mounter::~Mounter() {
    unmount();
}

bool Mounter::mount(const Settings& s, bool debug, const std::function<void(const std::string&)>& log,
                    std::string* error) {
    unmount();
    if (!fsys::Drive::loadWinFsp(error)) return false;
    if (s.folders.empty() && !s.testPatterns) {
        if (error) *error = "no MLV folder chosen";
        return false;
    }
    raw::setPixelMapFolders(raw::defaultPixelMapFolders());

    auto t0 = std::chrono::steady_clock::now();
    auto tree = std::make_shared<vfs::Tree>();
    if (!tree->build(s.folders, BuildOptionsOf(s), log, error)) return false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (tree->clipCount() == 0) {
        std::string where;
        for (const auto& f : s.folders) where += (where.empty() ? "" : ", ") + narrow(f);
        if (error) *error = "no MLV clips in " + where;
        return false;
    }

    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned readAhead = s.readAhead < 0 ? cpus : unsigned(s.readAhead);
    const unsigned cacheMb = s.cacheMb ? s.cacheMb : cacheAutoMb();
    auto cache = std::make_unique<vfs::FrameCache>(uint64_t(cacheMb) << 20, readAhead ? cpus : 0, readAhead);
    auto drive = std::make_unique<fsys::Drive>(tree, *cache);
    if (!drive->mount(normalizeDrive(s.drive), debug, error)) return false;

    std::wstring mountPoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = s;
        info_ = MountInfo{};
        info_.mountPoint = mountPoint = drive->mountPoint();
        info_.clips = tree->clipCount();
        info_.bytes = tree->totalBytes();
        info_.openMs = ms;
        info_.cacheMb = cacheMb;
        info_.readAhead = readAhead;
        tree_ = std::move(tree);
        cache_ = std::move(cache);
        drive_ = std::move(drive);
    }
    // The rescan thread always runs (for `mlvdrive rescan`); the folder watchers with `watch` on.
    watcher_ = std::make_unique<Watcher>(*this, s.folders, s.watch, mountPoint);
    std::lock_guard<std::mutex> lock(mutex_);
    info_.polling = watcher_->polling();
    return true;
}

void Mounter::stopWatching() {
    watcher_.reset();  // joins its threads, which may be rescanning: before taking the lock
}

void Mounter::unmount() {
    stopWatching();
    std::lock_guard<std::mutex> lock(mutex_);
    // The drive first (no more reads), then the cache (its workers), then the clips.
    if (drive_) drive_->unmount();
    drive_.reset();
    cache_.reset();
    tree_.reset();
}

bool Mounter::mounted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return drive_ != nullptr;
}

MountInfo Mounter::info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return info_;
}

vfs::FrameCache::Stats Mounter::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_ ? cache_->stats() : vfs::FrameCache::Stats{};
}

bool Mounter::rescan(RescanResult* result, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!drive_) {
        if (error) *error = "not mounted";
        return false;
    }
    RescanResult r;
    auto next = std::make_shared<vfs::Tree>();
    auto log = [&](const std::string& m) { r.skipped.push_back(m); };
    if (!next->build(settings_.folders, BuildOptionsOf(settings_), log, error, tree_.get())) return false;

    // Clips by where they came from: added, gone, or changed (opened again: a new entry).
    std::map<std::wstring, uint64_t> before, after;
    for (const auto& c : tree_->clips()) before[c->sourceKey] = c->id;
    for (const auto& c : next->clips()) after[c->sourceKey] = c->id;
    for (const auto& [key, id] : before) {
        auto it = after.find(key);
        if (it == after.end()) ++r.removed;
        else if (it->second != id) ++r.changed;
        if (it == after.end() || it->second != id) cache_->forget(id);  // its frames won't be asked for again
    }
    for (const auto& [key, id] : after)
        if (!before.count(key)) ++r.added;

    drive_->update(next, vfs::Tree::changes(*tree_, *next));
    tree_ = std::move(next);
    info_.clips = tree_->clipCount();
    info_.bytes = tree_->totalBytes();
    r.info = info_;
    if (result) *result = std::move(r);
    return true;
}

}  // namespace app
