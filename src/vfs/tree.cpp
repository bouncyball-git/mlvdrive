#include "vfs/tree.h"

#include "util/file.h"
#include "vfs/test_patterns.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace vfs {

namespace fs = std::filesystem;

int compareNames(const wchar_t* a, size_t aLen, const wchar_t* b, size_t bLen) {
#ifdef _WIN32
    int r = CompareStringOrdinal(a, int(aLen), b, int(bLen), TRUE);
    return r == CSTR_LESS_THAN ? -1 : r == CSTR_GREATER_THAN ? 1 : 0;
#else
    for (size_t i = 0; i < aLen && i < bLen; ++i) {
        wint_t x = std::towupper(wint_t(a[i])), y = std::towupper(wint_t(b[i]));
        if (x != y) return x < y ? -1 : 1;
    }
    return aLen < bLen ? -1 : aLen > bLen ? 1 : 0;
#endif
}

namespace {

int compareNames(const std::wstring& a, const std::wstring& b) {
    return vfs::compareNames(a.data(), a.size(), b.data(), b.size());
}

bool isMlv(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    return compareNames(ext, L".mlv") == 0;
}

// A clip's recording time (RTCI, the camera's local time) as a FILETIME, or 0 without one.
uint64_t recordingTime(const mlv::Clip& clip) {
#ifdef _WIN32
    const auto& r = clip.rtci;
    if (r.type[0] == 0 || r.tm_year < 70) return 0;
    SYSTEMTIME local{};
    local.wYear = WORD(1900 + r.tm_year);
    local.wMonth = WORD(r.tm_mon + 1);
    local.wDay = WORD(r.tm_mday);
    local.wHour = WORD(r.tm_hour);
    local.wMinute = WORD(r.tm_min);
    local.wSecond = WORD(r.tm_sec);
    SYSTEMTIME utc;
    FILETIME ft;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return 0;
    return uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
#else
    (void)clip;
    return 0;
#endif
}

// A file's last write time as a FILETIME.
uint64_t fileTime(const fs::path& p) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &data)) return 0;
    return uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32 | data.ftLastWriteTime.dwLowDateTime;
#else
    (void)p;
    return 0;
#endif
}

uint64_t now() {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return uint64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime;
#else
    return 0;
#endif
}

Dir* childDir(Dir* parent, const std::wstring& name) {
    for (auto& d : parent->dirs)
        if (compareNames(d->name, name) == 0) return d.get();
    return nullptr;
}

void sortTree(Dir* dir) {
    std::sort(dir->dirs.begin(), dir->dirs.end(),
              [](const std::unique_ptr<Dir>& a, const std::unique_ptr<Dir>& b) { return compareNames(a->name, b->name) < 0; });
    for (auto& d : dir->dirs) sortTree(d.get());
}

// A folder's time: its newest clip's (folders without clips are left out of the tree).
uint64_t datesFromClips(Dir* dir) {
    if (dir->clip) return dir->time;
    uint64_t newest = 0;
    for (auto& d : dir->dirs) newest = std::max(newest, datesFromClips(d.get()));
    if (newest) dir->time = newest;
    return dir->time;
}

// An MLV clip's frames, through its DNG writer.
class MlvSource : public FrameSource {
public:
    MlvSource(const mlv::Clip& clip, const dng::Writer& writer) : clip_(clip), writer_(writer) {}
    uint32_t frameCount() const override { return clip_.frameCount(); }
    uint64_t headerSize() const override { return writer_.headerSize(); }
    uint64_t fileSize() const override { return writer_.fileSize(); }
    uint64_t frameOffset(uint32_t frame) const override {
        return (clip_.frame(frame).timestamp - clip_.frame(0).timestamp) * 10;  // microseconds -> 100 ns
    }
    bool renderHeader(uint32_t frame, std::vector<uint8_t>& out) const override {
        return writer_.renderHeader(frame, out);
    }
    bool renderFrame(uint32_t frame, std::vector<uint8_t>& out, std::string* error) const override {
        thread_local dng::Writer::Scratch scratch;
        return writer_.renderFrame(frame, out, scratch, error);
    }

private:
    const mlv::Clip& clip_;
    const dng::Writer& writer_;
};

}  // namespace

std::wstring Tree::frameName(const ClipEntry& clip, uint32_t frame) {
    wchar_t digits[16];
    std::swprintf(digits, 16, L"_%06u.dng", frame);
    return clip.name + digits;
}

bool Tree::build(const std::wstring& sourceFolder, const BuildOptions& options,
                 const std::function<void(const std::string&)>& log, std::string* error) {
    const fs::path source(sourceFolder);
    std::error_code ec;
    // --- The clips: every .MLV under the folder (its .M00, .M01 ... are opened with it); no
    //     folder: none (the test patterns alone) ---
    std::vector<fs::path> paths;
    if (!sourceFolder.empty()) {
        if (!fs::is_directory(source, ec)) {
            if (error) *error = "not a folder: " + util::u8(source);
            return false;
        }
        fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            if (error) *error = "can't read " + util::u8(source) + ": " + ec.message();
            return false;
        }
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && isMlv(it->path())) paths.push_back(it->path());
        }
        std::sort(paths.begin(), paths.end());
    }

    // Opened on several threads: a clip is indexed by reading every block header of every chunk.
    std::vector<std::unique_ptr<ClipEntry>> opened(paths.size());
    std::atomic<size_t> next{0};
    std::mutex logMutex;
    auto work = [&] {
        for (size_t i; (i = next++) < paths.size();) {
            std::string why;
            auto clip = mlv::Clip::open(util::u8(paths[i]), &why);
            if (clip && clip->frameCount() == 0) why = "no frames";
            if (!clip || clip->frameCount() == 0) {
                std::lock_guard<std::mutex> lock(logMutex);
                if (log) log("skipped " + util::u8(paths[i]) + ": " + why);
                continue;
            }
            auto e = std::make_unique<ClipEntry>();
            e->name = paths[i].stem().wstring();
            e->time = recordingTime(*clip);
            if (!e->time) e->time = fileTime(paths[i]);
            if (options.fix) e->fixer = std::make_unique<raw::Fixer>(*clip, options.fixes);
            e->writer = std::make_unique<dng::Writer>(*clip, options.dng, e->fixer.get());
            // The audio, its time reference on frame 0's DNG timecode.
            e->wav = mlv::wavLayout(*clip, mlv::wavTimeReference(*clip, e->writer->timecodeFrame(0),
                                                                  e->writer->frameRate()));
            e->source = std::make_unique<MlvSource>(*clip, *e->writer);
            e->clip = std::move(clip);
            opened[i] = std::move(e);
        }
    };
    const size_t threads = std::min<size_t>(paths.size(), std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();

    // --- The tree ---
    root_ = Dir{};
    root_.time = now();
    uint64_t nextId = 1;
    root_.id = nextId++;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!opened[i]) continue;
        ClipEntry* e = opened[i].get();
        if (e->source->fileSize() == 0) {
            if (error) *error = "the drive needs a DNG format of fixed size (16-bit or packed)";
            return false;
        }
        Dir* parent = &root_;
        for (const auto& part : paths[i].parent_path().lexically_relative(source)) {
            std::wstring name = part.wstring();
            if (name == L"." || name.empty()) continue;
            Dir* d = childDir(parent, name);
            if (!d) {
                parent->dirs.push_back(std::make_unique<Dir>());
                d = parent->dirs.back().get();
                d->name = name;
                d->id = nextId++;
            }
            parent = d;
        }
        addClip(parent, std::move(opened[i]), &nextId);
    }
    if (options.testPatterns) {
        auto e = std::make_unique<ClipEntry>();
        e->name = L"TestPatterns";
        e->time = now();
        e->source = makeTestPatterns(options.dng.timecode == dng::Timecode::TimeOfDay);
        addClip(&root_, std::move(e), &nextId);
    }
    sortTree(&root_);
    datesFromClips(&root_);
    return true;
}

void Tree::addClip(Dir* parent, std::unique_ptr<ClipEntry> entry, uint64_t* nextId) {
    ClipEntry* e = entry.get();
    // Two clips named alike in one folder (A.MLV, and a folder "A" of clips): "A (2)".
    std::wstring name = e->name;
    for (int n = 2; childDir(parent, name); ++n) name = e->name + L" (" + std::to_wstring(n) + L")";
    e->name = name;
    parent->dirs.push_back(std::make_unique<Dir>());
    Dir* d = parent->dirs.back().get();
    d->name = name;
    d->time = e->time;
    d->clip = e;
    e->id = *nextId;
    d->id = *nextId;
    *nextId += 2 + e->source->frameCount();
    totalBytes_ += e->source->fileSize() * e->source->frameCount() + e->wav.size();
    clips_.push_back(std::move(entry));
}

Node Tree::find(const wchar_t* path) const {
    Node n;
    const Dir* dir = &root_;
    const wchar_t* p = path;
    while (*p == L'\\') ++p;
    while (*p) {
        const wchar_t* end = p;
        while (*end && *end != L'\\') ++end;
        const size_t len = size_t(end - p);
        const wchar_t* rest = end;
        while (*rest == L'\\') ++rest;

        if (dir->clip) {
            // A frame: NAME_000123.dng, or the audio: NAME.wav; the last part of the path.
            if (*rest) return n;
            const ClipEntry& c = *dir->clip;
            if (c.hasWav()) {
                const std::wstring wav = wavName(c);
                if (compareNames(p, len, wav.data(), wav.size()) == 0) {
                    n.kind = Node::Audio;
                    n.dir = dir;
                    return n;
                }
            }
            const size_t prefix = c.name.size() + 1;
            if (len < prefix + 4 + 1 || compareNames(p, c.name.size(), c.name.data(), c.name.size()) != 0 ||
                p[c.name.size()] != L'_')
                return n;
            uint64_t frame = 0;
            size_t i = prefix;
            for (; i < len && *(p + i) >= L'0' && *(p + i) <= L'9' && i - prefix < 10; ++i) frame = frame * 10 + (p[i] - L'0');
            if (i == prefix || frame >= c.source->frameCount()) return n;
            const std::wstring expected = frameName(c, uint32_t(frame));  // leading zeros, extension
            if (compareNames(p, len, expected.data(), expected.size()) != 0) return n;
            n.kind = Node::Frame;
            n.dir = dir;
            n.frame = uint32_t(frame);
            return n;
        }

        // A folder: binary search (sorted case-insensitively).
        auto it = std::lower_bound(dir->dirs.begin(), dir->dirs.end(), 0,
                                   [&](const std::unique_ptr<Dir>& d, int) {
                                       return compareNames(d->name.data(), d->name.size(), p, len) < 0;
                                   });
        if (it == dir->dirs.end() || compareNames((*it)->name.data(), (*it)->name.size(), p, len) != 0) return n;
        dir = it->get();
        p = rest;
    }
    n.kind = Node::Directory;
    n.dir = dir;
    return n;
}

}  // namespace vfs
