// The drive on Windows: the tree (vfs::Tree) as a read-only WinFsp volume, through WinFsp's native
// API. A DNG's bytes are made when the file is read: its header alone for reads of the start, the
// whole file (frame decoded and corrected, through the frame cache, which reads ahead) for
// anything further; an open file holds it until its handle is closed.
#pragma once

#include "vfs/frame_cache.h"
#include "vfs/tree.h"

#include <memory>
#include <string>

namespace fsys {

class Drive {
public:
    Drive(const vfs::Tree& tree, vfs::FrameCache& cache);
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

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace fsys
