// One mount: the clips under the settings' folder (and the test patterns) as a drive, with its
// frame cache. Used by the console program and the tray app. Not thread-safe: one caller at a time.
#pragma once

#include "app/settings.h"
#include "vfs/frame_cache.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fsys {
class Drive;
}
namespace vfs {
class Tree;
}

namespace app {

struct MountInfo {
    std::wstring mountPoint;  // "M:"
    size_t clips = 0;         // clip folders (the test patterns count as one)
    uint64_t bytes = 0;       // the files' total size
    double openMs = 0;        // finding and opening the clips
    unsigned cacheMb = 0, readAhead = 0;
};

class Mounter {
public:
    Mounter();
    ~Mounter();  // unmounts
    Mounter(const Mounter&) = delete;
    Mounter& operator=(const Mounter&) = delete;

    // False with `error`: WinFsp missing, the folder unreadable, no clips, the letter in use...
    // `log` hears about clips that can't be opened.
    bool mount(const Settings& settings, bool debug, const std::function<void(const std::string&)>& log,
               std::string* error);
    void unmount();
    bool mounted() const { return drive_ != nullptr; }
    const MountInfo& info() const { return info_; }
    vfs::FrameCache::Stats stats() const;

private:
    std::unique_ptr<vfs::Tree> tree_;
    std::unique_ptr<vfs::FrameCache> cache_;
    std::unique_ptr<fsys::Drive> drive_;
    MountInfo info_;
};

// The named event a mount waits on, after its mount point: `mlvdrive unmount M:` sets it.
std::wstring stopEventName(std::wstring mountPoint);

std::string narrow(const std::wstring& w);  // UTF-16 -> UTF-8
std::wstring widen(const std::string& s);   // UTF-8 -> UTF-16

}  // namespace app
