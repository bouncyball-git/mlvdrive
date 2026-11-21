// The drive on Windows: the tree (vfs::Tree) as a read-only WinFsp volume, through WinFsp's native
// API. A DNG's bytes are made when the file is read: its header alone for reads of the start, the
// whole file (frame decoded and corrected, through the frame cache, which reads ahead) for
// anything further; an open file holds it until its handle is closed.
//
// The tree can be replaced while mounted (update()): what is opened from then on comes from the
// new one, what is open keeps the tree it was opened in until it is closed, and Windows is told
// what changed (Explorer refreshes; its caches of names and sizes are dropped for those).
#pragma once

#include "vfs/frame_cache.h"
#include "vfs/tree.h"

#include <memory>
#include <string>
#include <vector>

namespace fsys {

class Drive {
public:
    Drive(std::shared_ptr<const vfs::Tree> tree, vfs::FrameCache& cache);
    ~Drive();
    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    // Loads WinFsp's DLL (from its install folder). False if WinFsp isn't installed.
    static bool loadWinFsp(std::string* error);

    // Mounts at `mountPoint` ("M:", or a folder that doesn't exist yet; empty: the next free
    // letter from Z: down). `debug` logs every request to stderr.
    bool mount(const std::wstring& mountPoint, bool debug, std::string* error);
    void unmount();
    std::wstring mountPoint() const;

    // Shows `tree` from now on; `changes` (Tree::changes from the one shown) are notified.
    void update(std::shared_ptr<const vfs::Tree> tree, const std::vector<vfs::Change>& changes);

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace fsys
