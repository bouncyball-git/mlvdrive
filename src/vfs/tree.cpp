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

// Index numbers of clips: unique for the drive's life (a rebuild gives new clips new ones). Folder
// numbers come from their path (the top bit set: they never meet a clip's).
std::atomic<uint64_t> g_nextClipId{1};

uint64_t takeIds(uint64_t count) { return g_nextClipId.fetch_add(count); }

uint64_t hashText(const std::wstring& text) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (wchar_t ch : text) {
        h ^= uint64_t(std::towupper(wint_t(ch)));
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t dirId(const std::wstring& path) { return (1ull << 63) | hashText(path); }

// The size and date of a clip's files (.MLV, .M00 ... .M98, as mlv::Clip opens them): what tells a
// rebuild it changed.
uint64_t clipSignature(const fs::path& mlv) {
    std::error_code ec;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    auto add = [&](const fs::path& p) {
        const uint64_t size = fs::file_size(p, ec);
        if (ec) return false;
        mix(size);
        mix(fileTime(p));
        return true;
    };
    add(mlv);
    for (int n = 0; n < 99; ++n) {
        wchar_t ext[8];
        std::swprintf(ext, 8, L".M%02d", n);
        fs::path chunk = mlv;
        if (!add(chunk.replace_extension(ext))) break;
    }
    return h;
}

std::wstring upperPath(const fs::path& p) {
    std::error_code ec;
    std::wstring key = fs::weakly_canonical(p, ec).wstring();
    if (ec) key = p.wstring();
    for (auto& ch : key) ch = wchar_t(std::towupper(wint_t(ch)));
    return key;
}

const wchar_t kTestPatternsKey[] = L"<test patterns>";

// Opens a clip as a folder's entry (name: the .MLV's, set again once placed); null if it can't be.
std::shared_ptr<ClipEntry> openEntry(const fs::path& path, const std::wstring& key, uint64_t signature,
                                     const BuildOptions& options, std::string* why) {
    auto clip = mlv::Clip::open(util::u8(path), why);
    if (clip && clip->frameCount() == 0) *why = "no frames";
    if (!clip || clip->frameCount() == 0) return nullptr;
    auto e = std::make_shared<ClipEntry>();
    e->name = path.stem().wstring();
    e->time = recordingTime(*clip);
    if (!e->time) e->time = fileTime(path);
    if (options.fix) e->fixer = std::make_unique<raw::Fixer>(*clip, options.fixes);
    e->writer = std::make_unique<dng::Writer>(*clip, options.dng, e->fixer.get());
    // The audio, its time reference on frame 0's DNG timecode.
    e->wav = mlv::wavLayout(*clip, mlv::wavTimeReference(*clip, e->writer->timecodeFrame(0), e->writer->frameRate()));
    e->source = std::make_unique<MlvSource>(*clip, *e->writer);
    e->clip = std::move(clip);
    e->sourceKey = key;
    e->signature = signature;
    e->id = takeIds(2 + uint64_t(e->source->frameCount()));
    return e;
}

// A clip folder's name in `parent`: its own, or "A (2)" when taken (A.MLV beside a folder "A").
std::wstring freeName(Dir* parent, const std::wstring& name) {
    std::wstring n = name;
    for (int i = 2; childDir(parent, n); ++i) n = name + L" (" + std::to_wstring(i) + L")";
    return n;
}

// The files of a clip folder that differ between two entries shown under the same name.
void clipChanges(const ClipEntry& a, const ClipEntry& b, const std::wstring& dir, std::vector<Change>* out) {
    const uint32_t na = a.source->frameCount(), nb = b.source->frameCount();
    for (uint32_t i = 0; i < std::max(na, nb); ++i) {
        const Change::Action act = i >= na ? Change::Added : i >= nb ? Change::Removed : Change::Modified;
        out->push_back({act, false, dir + L"\\" + Tree::frameName(i < nb ? b : a, i)});
    }
    if (a.hasWav() || b.hasWav()) {
        const Change::Action act = !a.hasWav() ? Change::Added : !b.hasWav() ? Change::Removed : Change::Modified;
        out->push_back({act, false, dir + L"\\" + Tree::wavName(b.hasWav() ? b : a)});
    }
}

void dirChanges(const Dir& a, const Dir& b, const std::wstring& path, std::vector<Change>* out) {
    size_t i = 0, j = 0;
    while (i < a.dirs.size() || j < b.dirs.size()) {
        const int c = i == a.dirs.size() ? 1 : j == b.dirs.size() ? -1 : compareNames(a.dirs[i]->name, b.dirs[j]->name);
        if (c < 0) {
            out->push_back({Change::Removed, true, path + L"\\" + a.dirs[i++]->name});
        } else if (c > 0) {
            out->push_back({Change::Added, true, path + L"\\" + b.dirs[j++]->name});
        } else {
            const Dir& x = *a.dirs[i++];
            const Dir& y = *b.dirs[j++];
            const std::wstring sub = path + L"\\" + y.name;
            if (bool(x.clip) != bool(y.clip)) {  // a clip's folder became a plain one, or back
                out->push_back({Change::Removed, true, sub});
                out->push_back({Change::Added, true, sub});
            } else if (x.clip) {
                if (x.clip != y.clip) {  // the clip changed (or another took its name)
                    clipChanges(*x.clip, *y.clip, sub, out);
                    out->push_back({Change::Modified, true, sub});
                }
            } else {
                dirChanges(x, y, sub, out);
            }
        }
    }
}

}  // namespace

std::wstring Tree::frameName(const ClipEntry& clip, uint32_t frame) {
    wchar_t digits[16];
    std::swprintf(digits, 16, L"_%06u.dng", frame);
    return clip.name + digits;
}

std::vector<Change> Tree::changes(const Tree& before, const Tree& after) {
    std::vector<Change> out;
    dirChanges(before.root_, after.root_, L"", &out);
    return out;
}

bool Tree::build(const std::vector<std::wstring>& folders, const BuildOptions& options,
                 const std::function<void(const std::string&)>& log, std::string* error, const Tree* previous) {
    std::error_code ec;
    // --- The clips: every .MLV under each folder (its .M00, .M01 ... are opened with it), with
    //     its folder relative to that one; a clip found twice (a folder listed twice, or inside
    //     another) counts once. A folder that can't be read is reported and left out. ---
    std::vector<fs::path> paths, rels;
    std::vector<std::wstring> keys;  // the clips' full paths, upper case
    for (const std::wstring& folder : folders) {
        if (folder.empty()) continue;
        const fs::path source(folder);
        fs::recursive_directory_iterator it;
        if (fs::is_directory(source, ec)) it = fs::recursive_directory_iterator(source, fs::directory_options::skip_permission_denied, ec);
        else ec = std::make_error_code(std::errc::not_a_directory);
        if (ec) {
            if (log) log("skipped folder " + util::u8(source) + ": " + ec.message());
            ec.clear();
            continue;
        }
        std::vector<fs::path> found;
        for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && isMlv(it->path())) found.push_back(it->path());
        }
        ec.clear();
        std::sort(found.begin(), found.end());
        for (const fs::path& p : found) {
            std::wstring key = upperPath(p);
            if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
            keys.push_back(key);
            paths.push_back(p);
            rels.push_back(p.parent_path().lexically_relative(source));
        }
    }

    // --- Taken over from the previous tree while their files are as they were; the rest opened
    //     on several threads (a clip is indexed by reading every block header of every chunk) ---
    std::map<std::wstring, std::shared_ptr<ClipEntry>> before;
    if (previous)
        for (const auto& e : previous->clips_) before[e->sourceKey] = e;
    std::vector<std::shared_ptr<ClipEntry>> entries(paths.size());
    std::vector<uint64_t> signatures(paths.size());
    std::vector<size_t> toOpen;
    for (size_t i = 0; i < paths.size(); ++i) {
        signatures[i] = clipSignature(paths[i]);
        auto it = before.find(keys[i]);
        if (it != before.end() && it->second->signature == signatures[i]) entries[i] = it->second;
        else toOpen.push_back(i);
    }
    std::atomic<size_t> next{0};
    std::mutex logMutex;
    auto work = [&] {
        for (size_t k; (k = next++) < toOpen.size();) {
            const size_t i = toOpen[k];
            std::string why;
            entries[i] = openEntry(paths[i], keys[i], signatures[i], options, &why);
            if (!entries[i]) {
                std::lock_guard<std::mutex> lock(logMutex);
                if (log) log("skipped " + util::u8(paths[i]) + ": " + why);
            }
        }
    };
    const size_t threads = std::min<size_t>(toOpen.size(), std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();

    // --- The tree ---
    root_ = Dir{};
    root_.time = now();
    root_.id = dirId(L"\\");
    clips_.clear();
    totalBytes_ = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        if (!entries[i]) continue;
        if (entries[i]->source->fileSize() == 0) {
            if (error) *error = "the drive needs a DNG format of fixed size (16-bit or packed)";
            return false;
        }
        Dir* parent = &root_;
        std::wstring at;
        for (const auto& part : rels[i]) {
            std::wstring name = part.wstring();
            if (name == L"." || name.empty()) continue;
            at += L"\\" + name;
            Dir* d = childDir(parent, name);
            if (!d) {
                parent->dirs.push_back(std::make_unique<Dir>());
                d = parent->dirs.back().get();
                d->name = name;
                d->id = dirId(at);
            }
            parent = d;
        }
        // Its name here; an entry taken over under another name (a clash that came or went) is
        // opened again: its files' names are part of it.
        const std::wstring name = freeName(parent, paths[i].stem().wstring());
        if (entries[i]->name != name && entries[i]->clip && previous && before.count(keys[i]) &&
            before[keys[i]] == entries[i]) {
            std::string why;
            entries[i] = openEntry(paths[i], keys[i], signatures[i], options, &why);
            if (!entries[i]) continue;
        }
        // Only a new entry gets its name: one taken over has it already, and the drive is reading
        // it on other threads.
        if (entries[i]->name != name) entries[i]->name = name;
        addClip(parent, entries[i]);
    }
    if (options.testPatterns) {
        std::shared_ptr<ClipEntry> e;
        if (auto it = before.find(kTestPatternsKey); it != before.end() && it->second->name == freeName(&root_, L"TestPatterns"))
            e = it->second;
        if (!e) {
            e = std::make_shared<ClipEntry>();
            e->name = freeName(&root_, L"TestPatterns");
            e->time = now();
            e->source = makeTestPatterns(options.dng.timecode == dng::Timecode::TimeOfDay);
            e->sourceKey = kTestPatternsKey;
            e->id = takeIds(2 + uint64_t(e->source->frameCount()));
        }
        addClip(&root_, e);
    }
    sortTree(&root_);
    datesFromClips(&root_);
    return true;
}

void Tree::addClip(Dir* parent, std::shared_ptr<ClipEntry> e) {
    parent->dirs.push_back(std::make_unique<Dir>());
    Dir* d = parent->dirs.back().get();
    d->name = e->name;
    d->time = e->time;
    d->id = e->id;
    d->clip = e;
    totalBytes_ += e->source->fileSize() * e->source->frameCount() + e->wav.size();
    clips_.push_back(std::move(e));
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
