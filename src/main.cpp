// mlvdrive: shows the MLV clips under a folder as a drive of CinemaDNG sequences (console).
//
//   mlvdrive mount [MLV folder ...] [M:] [options]
//   mlvdrive unmount <M:>
//   mlvdrive rescan <M:>
//
// mount takes its defaults from the settings file (app/settings.h; the tray app writes it) and
// runs until Ctrl+C, the console is closed or `mlvdrive unmount` is run, then unmounts.
#include "app/mounter.h"
#include "app/settings.h"
#include "parallel.h"
#include "util/file.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace {

HANDLE g_stop = nullptr;

BOOL WINAPI onConsoleEvent(DWORD) {
    SetEvent(g_stop);
    return TRUE;
}

int usage() {
    std::fprintf(stderr,
                 "MLV Drive " MLVDRIVE_VERSION ": MLV clips as a drive of CinemaDNG sequences\n"
                 "usage: mlvdrive mount [MLV folder ...] [M:] [options]\n"
                 "       mlvdrive unmount <M:>\n"
                 "       mlvdrive rescan <M:>      (looks at the folders again now)\n"
                 "  Shows every MLV clip under the folders (merged) as a folder of DNG files. The folders,\n"
                 "  the drive letter and the options default to the settings file:\n"
                 "    %s\n"
                 "  --dng=16         16-bit uncompressed DNG (the default)\n"
                 "  --dng=packed     uncompressed, packed to the clip's bit depth (smaller)\n"
                 "  --no-fix         no raw corrections (stripes, focus and bad pixels); --fix: with them\n"
                 "  --focus-pixels=auto|on|off   auto: cameras with focus pixels, clips with their own .fpm\n"
                 "  --bad-pixels=off|auto|aggressive|map   searched on frame 0, harder, or from a .bpm map\n"
                 "                   (maps: beside the clip as <clip>.fpm/.bpm or <camera>_<w>x<h>, then\n"
                 "                   %%APPDATA%%\\MLV Drive\\pixelmaps, then pixelmaps beside the program)\n"
                 "  --timecode=time  DNG timecode and WAV time reference: the time of day the clip was\n"
                 "                   recorded (the default; clips line up by when they were shot)\n"
                 "  --timecode=count the frames from the start of the clip (00:00:00:00)\n"
                 "  --test-patterns  add a TestPatterns folder: 12 test charts (colour bars, PLUGE, grey\n"
                 "                   scale, crosshatch, safe areas, zone plate...), 5 s each, 1080p25; the\n"
                 "                   MLV folder may then be left out\n"
                 "  --no-test-patterns  without them (whatever the settings say)\n"
                 "  --no-watch       don't watch the folders (new clips show after `mlvdrive rescan`)\n"
                 "  --cache=MB       finished frames kept in memory (auto: a quarter of the RAM, 512 MB\n"
                 "                   to 16 GB; the default)\n"
                 "  --read-ahead=N   frames made ahead of the one read (default: one per CPU; 0: none)\n"
                 "  --debug          log every file system request\n"
                 "  Ctrl+C or `mlvdrive unmount M:` unmounts.\n",
                 util::narrow(app::settingsPath()).c_str());
    return 2;
}

bool isDriveLetter(const std::wstring& s) {
    return (s.size() == 1 || (s.size() == 2 && s[1] == L':')) && std::iswalpha(wint_t(s[0]));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 2 && std::wstring(argv[1]) == L"--version") {
        std::printf("MLV Drive %s\n", MLVDRIVE_VERSION);
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"unmount") {
        const std::wstring at = app::normalizeDrive(argv[2]);
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, app::stopEventName(at).c_str());
        if (!e) {
            std::fprintf(stderr, "nothing mounted on %s by MLV Drive\n", util::narrow(at).c_str());
            return 1;
        }
        SetEvent(e);
        CloseHandle(e);
        std::printf("unmounting %s\n", util::narrow(at).c_str());
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"rescan") {
        const std::wstring at = app::normalizeDrive(argv[2]);
        HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, app::rescanEventName(at).c_str());
        if (!e) {
            std::fprintf(stderr, "nothing mounted on %s by MLV Drive\n", util::narrow(at).c_str());
            return 1;
        }
        SetEvent(e);
        CloseHandle(e);
        std::printf("rescanning %s\n", util::narrow(at).c_str());
        return 0;
    }
    if (argc < 2 || std::wstring(argv[1]) != L"mount") return usage();

    std::vector<std::string> warnings;
    app::Settings s = app::loadSettings(app::settingsPath(), &warnings);
    for (const auto& w : warnings) std::fprintf(stderr, "%s\n", w.c_str());

    std::vector<std::wstring> positional;
    bool debug = false;
    auto number = [](const std::wstring& a, size_t at, unsigned long* out) {
        wchar_t* end = nullptr;
        *out = std::wcstoul(a.c_str() + at, &end, 10);
        return end && *end == 0 && a.size() > at;
    };
    for (int i = 2; i < argc; ++i) {
        std::wstring a = argv[i];
        unsigned long v = 0;
        if (a == L"--cache=auto") {
            s.cacheMb = 0;
        } else if (a.rfind(L"--cache=", 0) == 0) {
            if (!number(a, 8, &v) || v < 16) return usage();
            s.cacheMb = unsigned(v);
        } else if (a.rfind(L"--read-ahead=", 0) == 0) {
            if (!number(a, 13, &v)) return usage();
            s.readAhead = int(v);
        } else if (a == L"--dng=16") s.format = dng::Output::Uncompressed16;
        else if (a == L"--dng=packed") s.format = dng::Output::Uncompressed;
        else if (a == L"--no-fix") s.rawFixes = false;
        else if (a == L"--fix") s.rawFixes = true;
        else if (a == L"--focus-pixels=auto") s.focusPixels = -1;
        else if (a == L"--focus-pixels=on") s.focusPixels = 1;
        else if (a == L"--focus-pixels=off") s.focusPixels = 0;
        else if (a == L"--bad-pixels=off") s.badPixels = 0;
        else if (a == L"--bad-pixels=auto") s.badPixels = 1;
        else if (a == L"--bad-pixels=aggressive") s.badPixels = 2;
        else if (a == L"--bad-pixels=map") s.badPixels = 3;
        else if (a == L"--timecode=time") s.timecode = dng::Timecode::TimeOfDay;
        else if (a == L"--timecode=count") s.timecode = dng::Timecode::FrameCount;
        else if (a == L"--test-patterns") s.testPatterns = true;
        else if (a == L"--no-test-patterns") s.testPatterns = false;
        else if (a == L"--no-watch") s.watch = false;
        else if (a == L"--watch") s.watch = true;
        else if (a == L"--debug") debug = true;
        else if (a.rfind(L"--", 0) == 0) return usage();
        else positional.push_back(a);
    }
    // [folder ...] [M:]: a drive letter last is the drive; folders given replace the settings'.
    if (!positional.empty() && isDriveLetter(positional.back())) {
        s.drive = app::normalizeDrive(positional.back());
        positional.pop_back();
    }
    if (!positional.empty()) s.folders = positional;
    if (s.folders.empty() && !s.testPatterns) {
        std::fprintf(stderr, "no MLV folder: give one, or set folder = in %s\n\n",
                     util::narrow(app::settingsPath()).c_str());
        return usage();
    }

    app::Mounter mounter;
    mounter.onRescan([](const app::RescanResult& r) {
        if (!r.added && !r.removed && !r.changed && r.skipped.empty()) return;
        std::printf("rescanned: %zu clip%s added, %zu removed, %zu changed; %zu clips (%.1f GB)\n", r.added,
                    r.added == 1 ? "" : "s", r.removed, r.changed, r.info.clips, r.info.bytes / 1e9);
        for (const auto& m : r.skipped) std::printf("  %s\n", m.c_str());
        std::fflush(stdout);
    });
    std::string error;
    if (!mounter.mount(s, debug, [](const std::string& m) { std::fprintf(stderr, "%s\n", m.c_str()); }, &error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const app::MountInfo info = mounter.info();
    std::string what;
    for (const auto& f : s.folders) what += (what.empty() ? "" : ", ") + util::narrow(f);
    if (what.empty()) what = "test patterns";
    std::printf("%s: %zu clip%s (%.1f GB of DNG), opened in %.0f ms\n", what.c_str(), info.clips,
                info.clips == 1 ? "" : "s", info.bytes / 1e9, info.openMs);
    std::printf("Mounted on %s (cache %u MB, read-ahead %u%s). Ctrl+C unmounts.\n", util::narrow(info.mountPoint).c_str(),
                info.cacheMb, info.readAhead,
                !s.watch ? "" : info.polling ? ", watching the folders, network ones also every 30 s"
                                             : ", watching the folders");
    std::fflush(stdout);

    g_stop = CreateEventW(nullptr, TRUE, FALSE, app::stopEventName(info.mountPoint).c_str());
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    WaitForSingleObject(g_stop, INFINITE);

    std::printf("Unmounting.\n");
    const auto st = mounter.stats();
    mounter.unmount();
    std::printf("frames: %llu made for a reader, %llu found made, %llu waited for; read-ahead made %llu, read %llu, "
                "dropped %llu\n",
                (unsigned long long)st.misses, (unsigned long long)st.hits, (unsigned long long)st.waits,
                (unsigned long long)st.readAhead, (unsigned long long)st.readAheadUsed, (unsigned long long)st.dropped);
    par::shutdown();
    return 0;
}
