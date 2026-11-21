// The drive on Windows: the tree (vfs::Tree) as a read-only WinFsp volume, through WinFsp's native
// API. A DNG's bytes are made when the file is read: its header alone for reads of the start, the
// whole file (frame decoded and corrected) for anything further; an open file keeps what it made
// until it is closed.
#pragma once

#include "vfs/tree.h"

#include <memory>
#include <string>

namespace fsys {

class Drive {
public:
    explicit Drive(const vfs::Tree& tree);
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
