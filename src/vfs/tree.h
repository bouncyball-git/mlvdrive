// The drive's contents: the MLV clips found under a folder, each shown as a folder of DNG files.
//
//   E:\mlv\M21-1642.MLV         ->  \M21-1642\M21-1642_000000.dng ... M21-1642_000260.dng,
//                                   \M21-1642\M21-1642.wav (if it has audio)
//   E:\mlv\day2\M22-0901.MLV    ->  \day2\M22-0901\M22-0901_000000.dng ...
//
// Several source folders merge at the root (a clip found in two counts once; names that clash get
// " (2)"). Subfolders holding clips are kept; everything else in a source folder is left out. Every
// name, size and date is known once the clips are opened: no frame is made to list the tree.
// With the test patterns on, a clip folder "TestPatterns" of generated frames is added too.
//
// A tree doesn't change once built, so any number of threads may read it. When the folders change,
// a new tree is built from the old one (rebuild): clips whose files are as they were are taken over
// as they are (the same ClipEntry, its frames still cached), only new or changed ones are opened;
// changes() lists what differs, for telling Windows.
#pragma once

#include "dng/dng_writer.h"
#include "mlv/clip.h"
#include "mlv/wav.h"
#include "raw/rawfix.h"
#include "vfs/source.h"

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
    // An MLV clip's (null for the test patterns):
    std::unique_ptr<mlv::Clip> clip;
    std::unique_ptr<raw::Fixer> fixer;
    std::unique_ptr<dng::Writer> writer;
    mlv::WavLayout wav;  // its audio as a WAV file (empty header: none)
    std::unique_ptr<FrameSource> source;  // what the DNG files are made from (uses the above)
    uint64_t time = 0;  // when the clip was recorded (or the file's date)
    // Unique for the drive's whole life (never reused, also across rebuilds): the frame cache's
    // key, and the index numbers: the folder's is id; frame N's id + 1 + N, the WAV's id + 1 +
    // frame count.
    uint64_t id = 0;
    // Where it came from (the .MLV's full path, upper case) and the size and date of its files:
    // a rebuild takes the entry over while they are the same.
    std::wstring sourceKey;
    uint64_t signature = 0;
    bool hasWav() const { return !wav.header.empty(); }
};

struct Dir {
    std::wstring name;
    uint64_t time = 0;
    uint64_t id = 0;
    std::vector<std::unique_ptr<Dir>> dirs;  // sorted by name, case-insensitively
    std::shared_ptr<ClipEntry> clip;         // set: a clip's folder (holding its DNGs, no dirs)
};

// What a path names.
struct Node {
    enum Kind { None, Directory, Frame, Audio } kind = None;
    const Dir* dir = nullptr;   // Directory: the folder; Frame, Audio: its clip's folder
    uint32_t frame = 0;         // Frame: the frame index
};

struct BuildOptions {
    dng::Options dng;
    raw::FixSettings fixes;
    bool fix = true;
    bool testPatterns = false;  // add the "TestPatterns" clip folder
};

// A difference between two trees, for Windows' change notifications.
struct Change {
    enum Action { Added, Removed, Modified } action;
    bool directory;
    std::wstring path;  // "\day2\M22-0901\M22-0901_000012.dng"
};

class Tree {
public:
    // Finds and opens the clips under the folders, their contents merged at the root. `log` is
    // told about folders that can't be read and clips that can't be opened (both left out).
    // `previous`: a tree built before with the same options, whose unchanged clips are taken over.
    // False (with `error`) only for settings the drive can't use.
    bool build(const std::vector<std::wstring>& folders, const BuildOptions& options,
               const std::function<void(const std::string&)>& log, std::string* error,
               const Tree* previous = nullptr);

    const Dir& root() const { return root_; }
    size_t clipCount() const { return clips_.size(); }
    uint64_t totalBytes() const { return totalBytes_; }
    const std::vector<std::shared_ptr<ClipEntry>>& clips() const { return clips_; }

    // A path as WinFsp passes it ("\M21-1642\M21-1642_000012.dng"; "\" is the root).
    Node find(const wchar_t* path) const;

    // What differs from `before` to `after`: folders and files added, removed or changed.
    static std::vector<Change> changes(const Tree& before, const Tree& after);

    // A frame's file name ("M21-1642_000012.dng"), the audio's ("M21-1642.wav").
    static std::wstring frameName(const ClipEntry& clip, uint32_t frame);
    static std::wstring wavName(const ClipEntry& clip) { return clip.name + L".wav"; }

private:
    void addClip(Dir* parent, std::shared_ptr<ClipEntry> entry);

    Dir root_;
    std::vector<std::shared_ptr<ClipEntry>> clips_;
    uint64_t totalBytes_ = 0;
};

// Case-insensitive (ordinal) comparison, as Windows compares file names.
int compareNames(const wchar_t* a, size_t aLen, const wchar_t* b, size_t bLen);

}  // namespace vfs
