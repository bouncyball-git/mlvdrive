// The drive's contents: the MLV clips found under a folder, each shown as a folder of DNG files.
//
//   E:\mlv\M21-1642.MLV         ->  \M21-1642\M21-1642_000000.dng ... M21-1642_000260.dng
//   E:\mlv\day2\M22-0901.MLV    ->  \day2\M22-0901\M22-0901_000000.dng ...
//
// Subfolders holding clips are kept; everything else in the source folder is left out. Every
// name, size and date is known once the clips are opened: no frame is made to list the tree.
// The tree doesn't change after it is built, so any number of threads may read it.
#pragma once

#include "dng/dng_writer.h"
#include "mlv/clip.h"
#include "raw/rawfix.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace vfs {

// Times are Windows FILETIME values (100 ns since 1601, UTC).
struct ClipEntry {
    std::wstring name;  // the folder's name and the files' prefix ("M21-1642")
    std::unique_ptr<mlv::Clip> clip;
    std::unique_ptr<raw::Fixer> fixer;
    std::unique_ptr<dng::Writer> writer;
    uint64_t time = 0;  // when the clip was recorded (or the file's date)
    uint64_t id = 0;    // index numbers: the folder's; frame N's file is id + 1 + N
};

struct Dir {
    std::wstring name;
    uint64_t time = 0;
    uint64_t id = 0;
    std::vector<std::unique_ptr<Dir>> dirs;  // sorted by name, case-insensitively
    ClipEntry* clip = nullptr;               // set: a clip's folder (holding its DNGs, no dirs)
};

// What a path names.
struct Node {
    enum Kind { None, Directory, Frame } kind = None;
    const Dir* dir = nullptr;   // Directory: the folder; Frame: its clip's folder
    uint32_t frame = 0;         // Frame: the frame index
};

struct BuildOptions {
    dng::Options dng;
    raw::FixSettings fixes;
    bool fix = true;
};

class Tree {
public:
    // Finds and opens the clips under `sourceFolder`. `log` is told about clips that can't be
    // opened. False (with `error`) if the folder can't be read.
    bool build(const std::wstring& sourceFolder, const BuildOptions& options,
               const std::function<void(const std::string&)>& log, std::string* error);

    const Dir& root() const { return root_; }
    size_t clipCount() const { return clips_.size(); }
    uint64_t totalBytes() const { return totalBytes_; }

    // A path as WinFsp passes it ("\M21-1642\M21-1642_000012.dng"; "\" is the root).
    Node find(const wchar_t* path) const;

    // A frame's file name ("M21-1642_000012.dng").
    static std::wstring frameName(const ClipEntry& clip, uint32_t frame);

private:
    Dir root_;
    std::vector<std::unique_ptr<ClipEntry>> clips_;
    uint64_t totalBytes_ = 0;
};

// Case-insensitive (ordinal) comparison, as Windows compares file names.
int compareNames(const wchar_t* a, size_t aLen, const wchar_t* b, size_t bLen);

}  // namespace vfs
