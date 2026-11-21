// One mount: the clips under the settings' folders (and the test patterns) as a drive, with its
// frame cache. Used by the console program and the tray app.
//
// While mounted, the folders can be looked at again (rescan()): clips added, removed or changed
// show up without unmounting (open files keep working; Windows is told what changed). With
// `watch` on, that happens by itself: the folders are watched, and a rescan follows 2 seconds after
// they last changed (so a clip being copied shows once it is complete). Folders whose changes
// Windows may not report (on a network drive or share, or that can't be watched) are also looked
// at every 30 seconds; local ones aren't (that would keep sleeping disks awake). `mlvdrive rescan
// M:` asks for one at once.
//
// mount(), unmount() and rescan() may be called from different threads (one at a time inside).
#pragma once

#include "app/settings.h"
#include "vfs/frame_cache.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace fsys {
class Drive;
}
namespace vfs {
class Tree;
}

namespace app {

struct MountInfo {
    std::wstring mountPoint;  // "M:"
    bool polling = false;     // some folders are also looked at every 30 seconds
    size_t clips = 0;         // clip folders (the test patterns count as one)
    uint64_t bytes = 0;       // the files' total size
    double openMs = 0;        // finding and opening the clips
    unsigned cacheMb = 0, readAhead = 0;
};

struct RescanResult {
    size_t added = 0, removed = 0, changed = 0;  // clips
    std::vector<std::string> skipped;            // folders or clips that couldn't be read
    MountInfo info;                              // after it
    bool automatic = false;                      // started by the folders changing (or by `mlvdrive rescan`)
};

class Mounter {
public:
    Mounter();
    ~Mounter();  // unmounts
    Mounter(const Mounter&) = delete;
    Mounter& operator=(const Mounter&) = delete;

    // False with `error`: WinFsp missing, the folders unreadable, no clips, the letter in use...
    // `log` hears about clips that can't be opened.
    bool mount(const Settings& settings, bool debug, const std::function<void(const std::string&)>& log,
               std::string* error);
    void unmount();
    bool mounted() const;
    MountInfo info() const;
    vfs::FrameCache::Stats stats() const;

    // Looks at the folders again and shows what changed. False (with `error`) if not mounted.
    bool rescan(RescanResult* result, std::string* error);

    // Called after each rescan started by the folders changing or `mlvdrive rescan` (on a thread
    // of the mounter's own); set before mount().
    void onRescan(std::function<void(const RescanResult&)> callback) { onRescan_ = std::move(callback); }

private:
    class Watcher;
    void stopWatching();

    mutable std::mutex mutex_;
    Settings settings_;
    std::shared_ptr<vfs::Tree> tree_;
    std::unique_ptr<vfs::FrameCache> cache_;
    std::unique_ptr<fsys::Drive> drive_;
    MountInfo info_;
    std::function<void(const RescanResult&)> onRescan_;
    std::unique_ptr<Watcher> watcher_;
};

// The named events a mount listens to, after its mount point: `mlvdrive unmount M:` sets the
// first (the program that mounted waits on it), `mlvdrive rescan M:` the second (the mounter
// rescans).
std::wstring stopEventName(std::wstring mountPoint);
std::wstring rescanEventName(std::wstring mountPoint);

std::string narrow(const std::wstring& w);  // UTF-16 -> UTF-8
std::wstring widen(const std::string& s);   // UTF-8 -> UTF-16

}  // namespace app
