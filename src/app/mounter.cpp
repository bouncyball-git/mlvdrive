#include "app/mounter.h"

#include "fs/winfsp_fs.h"
#include "vfs/tree.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace app {

std::wstring stopEventName(std::wstring mountPoint) {
    for (auto& ch : mountPoint) {
        if (ch == L':' || ch == L'\\' || ch == L'/') ch = L'_';
        else ch = wchar_t(std::towupper(wint_t(ch)));
    }
    return L"Local\\MLVDrive-stop-" + mountPoint;
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

Mounter::Mounter() = default;

Mounter::~Mounter() {
    unmount();
}

bool Mounter::mount(const Settings& s, bool debug, const std::function<void(const std::string&)>& log,
                    std::string* error) {
    unmount();
    if (!fsys::Drive::loadWinFsp(error)) return false;
    if (s.folder.empty() && !s.testPatterns) {
        if (error) *error = "no MLV folder chosen";
        return false;
    }

    vfs::BuildOptions options;
    options.dng.output = s.format;
    options.dng.timecode = s.timecode;
    options.fix = s.rawFixes;
    options.testPatterns = s.testPatterns;

    auto t0 = std::chrono::steady_clock::now();
    auto tree = std::make_unique<vfs::Tree>();
    if (!tree->build(s.folder, options, log, error)) return false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (tree->clipCount() == 0) {
        if (error) *error = "no MLV clips in " + narrow(s.folder);
        return false;
    }

    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned readAhead = s.readAhead < 0 ? cpus : unsigned(s.readAhead);
    auto cache = std::make_unique<vfs::FrameCache>(uint64_t(s.cacheMb) << 20, readAhead ? cpus : 0, readAhead);
    auto drive = std::make_unique<fsys::Drive>(*tree, *cache);
    if (!drive->mount(normalizeDrive(s.drive), debug, error)) return false;

    info_ = MountInfo{};
    info_.mountPoint = drive->mountPoint();
    info_.clips = tree->clipCount();
    info_.bytes = tree->totalBytes();
    info_.openMs = ms;
    info_.cacheMb = s.cacheMb;
    info_.readAhead = readAhead;
    tree_ = std::move(tree);
    cache_ = std::move(cache);
    drive_ = std::move(drive);
    return true;
}

void Mounter::unmount() {
    // The drive first (no more reads), then the cache (its workers), then the clips.
    if (drive_) drive_->unmount();
    drive_.reset();
    cache_.reset();
    tree_.reset();
}

vfs::FrameCache::Stats Mounter::stats() const {
    return cache_ ? cache_->stats() : vfs::FrameCache::Stats{};
}

}  // namespace app
