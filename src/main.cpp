// mlvdrive: shows the MLV clips under a folder as a drive of CinemaDNG sequences.
//
//   mlvdrive mount <MLV folder> [M:] [--dng=16|packed] [--no-fix] [--cache=MB] [--read-ahead=N]
//                  [--debug]
//   mlvdrive unmount <M:>
//
// mount runs until Ctrl+C, the console is closed or `mlvdrive unmount` is run, then unmounts.
#include "fs/winfsp_fs.h"
#include "parallel.h"
#include "vfs/frame_cache.h"
#include "vfs/tree.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

namespace {

HANDLE g_stop = nullptr;

BOOL WINAPI onConsoleEvent(DWORD) {
    SetEvent(g_stop);
    return TRUE;
}

// The event a mount waits on, named after its mount point: `mlvdrive unmount` sets it.
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

int usage() {
    std::fprintf(stderr,
                 "usage: mlvdrive mount <MLV folder> [M:] [options]\n"
                 "       mlvdrive unmount <M:>\n"
                 "  Shows every MLV clip under the folder as a folder of DNG files (default drive: the\n"
                 "  next free letter from Z: down). Ctrl+C unmounts.\n"
                 "  --dng=16         16-bit uncompressed DNG (the default)\n"
                 "  --dng=packed     uncompressed, packed to the clip's bit depth (smaller)\n"
                 "  --no-fix         no raw corrections (stripes, focus and bad pixels)\n"
                 "  --cache=MB       frames kept made (default 512)\n"
                 "  --read-ahead=N   frames made ahead of the one read (default: one per CPU; 0: none)\n"
                 "  --debug          log every file system request\n"
                 "  Ctrl+C or `mlvdrive unmount M:` unmounts.\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 3 && std::wstring(argv[1]) == L"unmount") {
        std::wstring at = argv[2];
        if (at.size() == 1) at += L':';
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, stopEventName(at).c_str());
        if (!e) {
            std::fprintf(stderr, "nothing mounted on %s by mlvdrive\n", narrow(at).c_str());
            return 1;
        }
        SetEvent(e);
        CloseHandle(e);
        std::printf("unmounting %s\n", narrow(at).c_str());
        return 0;
    }
    if (argc < 3 || std::wstring(argv[1]) != L"mount") return usage();

    std::vector<std::wstring> positional;
    vfs::BuildOptions options;
    bool debug = false;
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    unsigned long cacheMb = 512, readAhead = cpus;
    auto number = [](const std::wstring& a, size_t at, unsigned long* out) {
        wchar_t* end = nullptr;
        *out = std::wcstoul(a.c_str() + at, &end, 10);
        return end && *end == 0 && a.size() > at;
    };
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.rfind(L"--cache=", 0) == 0) {
            if (!number(a, 8, &cacheMb)) return usage();
        } else if (a.rfind(L"--read-ahead=", 0) == 0) {
            if (!number(a, 13, &readAhead)) return usage();
        } else if (a == L"--dng=16") options.dng.output = dng::Output::Uncompressed16;
        else if (a == L"--dng=packed") options.dng.output = dng::Output::Uncompressed;
        else if (a == L"--no-fix") options.fix = false;
        else if (a == L"--debug") debug = true;
        else if (a.rfind(L"--", 0) == 0) return usage();
        else positional.push_back(a);
    }
    if (positional.empty() || positional.size() > 2) return usage();
    const std::wstring source = positional[0];
    std::wstring mountPoint = positional.size() == 2 ? positional[1] : L"";
    if (mountPoint.size() == 1) mountPoint += L':';  // "M" -> "M:"

    std::string error;
    if (!fsys::Drive::loadWinFsp(&error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }

    auto t0 = std::chrono::steady_clock::now();
    vfs::Tree tree;
    if (!tree.build(source, options, [](const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); }, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (tree.clipCount() == 0) {
        std::fprintf(stderr, "no MLV clips under %s\n", narrow(source).c_str());
        return 1;
    }

    vfs::FrameCache cache(uint64_t(cacheMb) << 20, readAhead ? cpus : 0, unsigned(readAhead));
    fsys::Drive drive(tree, cache);
    if (!drive.mount(mountPoint, debug, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %zu clip%s (%.1f GB of DNG), opened in %.0f ms\n", narrow(source).c_str(), tree.clipCount(),
                tree.clipCount() == 1 ? "" : "s", tree.totalBytes() / 1e9, ms);
    std::printf("Mounted on %s (cache %lu MB, read-ahead %lu). Ctrl+C unmounts.\n", narrow(drive.mountPoint()).c_str(),
                cacheMb, readAhead);
    std::fflush(stdout);

    g_stop = CreateEventW(nullptr, TRUE, FALSE, stopEventName(drive.mountPoint()).c_str());
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    WaitForSingleObject(g_stop, INFINITE);

    std::printf("Unmounting.\n");
    drive.unmount();
    const auto s = cache.stats();
    std::printf("frames: %llu made for a reader, %llu found made, %llu waited for; read-ahead made %llu, read %llu, "
                "dropped %llu\n",
                (unsigned long long)s.misses, (unsigned long long)s.hits, (unsigned long long)s.waits,
                (unsigned long long)s.readAhead, (unsigned long long)s.readAheadUsed, (unsigned long long)s.dropped);
    par::shutdown();
    return 0;
}
