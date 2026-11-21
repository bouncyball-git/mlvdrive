// mlvdrive: shows the MLV clips under a folder as a drive of CinemaDNG sequences.
//
//   mlvdrive mount <MLV folder> [M:] [--dng=16|packed] [--no-fix] [--debug]
//
// Runs until Ctrl+C (or the console is closed), then unmounts.
#include "fs/winfsp_fs.h"
#include "parallel.h"
#include "vfs/tree.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

HANDLE g_stop = nullptr;

BOOL WINAPI onConsoleEvent(DWORD) {
    SetEvent(g_stop);
    return TRUE;
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
                 "usage: mlvdrive mount <MLV folder> [M:] [--dng=16|packed] [--no-fix] [--debug]\n"
                 "  Shows every MLV clip under the folder as a folder of DNG files (default drive: the\n"
                 "  next free letter from Z: down). Ctrl+C unmounts.\n"
                 "  --dng=16      16-bit uncompressed DNG (the default)\n"
                 "  --dng=packed  uncompressed, packed to the clip's bit depth (smaller)\n"
                 "  --no-fix      no raw corrections (stripes, focus and bad pixels)\n"
                 "  --debug       log every file system request\n");
    return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3 || std::wstring(argv[1]) != L"mount") return usage();

    std::vector<std::wstring> positional;
    vfs::BuildOptions options;
    bool debug = false;
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--dng=16") options.dng.output = dng::Output::Uncompressed16;
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

    fsys::Drive drive(tree);
    if (!drive.mount(mountPoint, debug, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %zu clip%s (%.1f GB of DNG), opened in %.0f ms\n", narrow(source).c_str(), tree.clipCount(),
                tree.clipCount() == 1 ? "" : "s", tree.totalBytes() / 1e9, ms);
    std::printf("Mounted on %s. Ctrl+C unmounts.\n", narrow(drive.mountPoint()).c_str());
    std::fflush(stdout);

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    WaitForSingleObject(g_stop, INFINITE);

    std::printf("Unmounting.\n");
    drive.unmount();
    par::shutdown();
    return 0;
}
