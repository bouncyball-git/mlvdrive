// mlvdrive on macOS: mounts, unmounts and rescans the FSKit drive (the engine runs in the module,
// MLVDriveFS.appex; this only asks the system). Installed in MLV Drive.app/Contents/MacOS.
//
//   mlvdrive mount [MLV folder] [--mount-point=PATH] [options]
//   mlvdrive unmount [mount point]
//   mlvdrive rescan
//
// mount without a folder shows the settings' folders (several, merged): it mounts MLV Drive's
// settings folder, which the module takes to mean that. The options become the module's
// -o key=value options, over the settings file.
#include "app/settings.h"
#include "util/file.h"

#include <notify.h>
#include <spawn.h>
#include <sys/wait.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern char** environ;

namespace {

int usage() {
    std::fprintf(stderr,
                 "MLV Drive " MLVDRIVE_VERSION ": MLV clips as a drive of CinemaDNG sequences\n"
                 "usage: mlvdrive mount [MLV folder] [--mount-point=PATH] [options]\n"
                 "       mlvdrive unmount [mount point]\n"
                 "       mlvdrive rescan          (looks at the folders again now)\n"
                 "  Shows every MLV clip under the folder (without one: the settings' folders, merged) as a\n"
                 "  folder of DNG files. The mount point and the options default to the settings file:\n"
                 "    %s\n"
                 "  --mount-point=PATH  where the drive shows (default ~/MLV Drive; made if missing)\n"
                 "  --dng=16         16-bit uncompressed DNG (the default)\n"
                 "  --dng=packed     uncompressed, packed to the clip's bit depth (smaller)\n"
                 "  --no-fix         no raw corrections (stripes, focus and bad pixels); --fix: with them\n"
                 "  --timecode=time  DNG timecode and WAV time reference: the time of day the clip was\n"
                 "                   recorded (the default); --timecode=count: from 00:00:00:00\n"
                 "  --test-patterns  add a TestPatterns folder of 12 test charts; --no-test-patterns\n"
                 "  --no-watch       don't watch the folders (new clips show after `mlvdrive rescan`)\n"
                 "  --cache=MB       finished frames kept in memory, 16 or more (default: a quarter of the RAM)\n"
                 "  --read-ahead=N   frames made ahead of the one read, 0 to 256 (default: one per CPU)\n"
                 "  Once: turn on \"MLV Drive\" in System Settings > General > Login Items & Extensions >\n"
                 "  File System Extensions, and for clips on the Desktop, in Documents or on external or\n"
                 "  network drives give MLV Drive Full Disk Access (Privacy & Security).\n",
                 util::narrow(app::settingsPath()).c_str());
    return 2;
}

// ~ and ~/... -> the home folder.
std::string expandHome(const std::string& path) {
    if (path == "~") return util::homeFolder();
    if (path.rfind("~/", 0) == 0) return util::homeFolder() + path.substr(1);
    return path;
}

// Runs a program, its output to ours; its exit code (-1: couldn't start it).
int run(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int mount(int argc, char** argv) {
    std::vector<std::string> warnings;
    const app::Settings s = app::loadSettings(app::settingsPath(), &warnings);
    for (const auto& w : warnings) std::fprintf(stderr, "%s\n", w.c_str());
    std::string folder, at = expandHome(util::narrow(s.drive));
    std::vector<std::string> options;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const char* prefix) { return a.substr(std::strlen(prefix)); };
        if (a.rfind("--mount-point=", 0) == 0) at = expandHome(value("--mount-point="));
        else if (a == "--dng=16") options.push_back("format=16");
        else if (a == "--dng=packed") options.push_back("format=packed");
        else if (a == "--no-fix") options.push_back("raw_fixes=0");
        else if (a == "--fix") options.push_back("raw_fixes=1");
        else if (a == "--timecode=time" || a == "--timecode=count") options.push_back("timecode=" + value("--timecode="));
        else if (a == "--test-patterns") options.push_back("test_patterns=1");
        else if (a == "--no-test-patterns") options.push_back("test_patterns=0");
        else if (a == "--watch") options.push_back("watch=1");
        else if (a == "--no-watch") options.push_back("watch=0");
        else if (a.rfind("--cache=", 0) == 0) options.push_back("cache_mb=" + value("--cache="));
        else if (a.rfind("--read-ahead=", 0) == 0) options.push_back("read_ahead=" + value("--read-ahead="));
        else if (a.rfind("--", 0) == 0 || !folder.empty()) return usage();
        else folder = a;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (folder.empty()) {
        // The settings folder: the module shows the settings' folders.
        if (s.folders.empty()) {
            std::fprintf(stderr, "no MLV folder: give one, or set folder = in %s\n\n",
                         util::narrow(app::settingsPath()).c_str());
            return usage();
        }
        folder = util::u8(util::u8(util::narrow(app::settingsPath())).parent_path());
    } else {
        folder = util::u8(fs::absolute(util::u8(folder), ec));
    }
    if (at.empty()) at = util::homeFolder() + "/MLV Drive";
    fs::create_directories(util::u8(at), ec);

    std::vector<std::string> args = {"/sbin/mount", "-F", "-t", "mlvdrive"};
    if (!options.empty()) {
        std::string o;
        for (const auto& x : options) o += (o.empty() ? "" : ",") + x;
        args.push_back("-o");
        args.push_back(o);
    }
    args.push_back(folder);
    args.push_back(at);
    const int code = run(args);
    if (code != 0) {
        std::fprintf(stderr,
                     "Not mounted. Is \"MLV Drive\" turned on (System Settings > General > Login Items & "
                     "Extensions > File System Extensions)? Are there MLV clips in %s, and can MLV Drive read "
                     "them (Full Disk Access)? The module's log: log show --last 5m --predicate "
                     "'subsystem == \"org.mlvdrive.fs\"'\n",
                     folder.c_str());
        return 1;
    }
    std::printf("Mounted on %s\n", at.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("MLV Drive %s\n", MLVDRIVE_VERSION);
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "mount") == 0) return mount(argc, argv);
    if (argc <= 3 && argc >= 2 && std::strcmp(argv[1], "unmount") == 0) {
        std::vector<std::string> w;
        const std::string at = argc == 3 ? expandHome(argv[2]) : expandHome(util::narrow(app::loadSettings(app::settingsPath(), &w).drive));
        return run({"/sbin/umount", at}) == 0 ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "rescan") == 0) {
        if (notify_post("org.mlvdrive.rescan") != NOTIFY_STATUS_OK) {
            std::fprintf(stderr, "can't reach MLV Drive\n");
            return 1;
        }
        std::printf("rescanning\n");
        return 0;
    }
    return usage();
}
