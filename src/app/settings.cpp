#include "app/settings.h"

#include "util/file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#else
#include <unistd.h>
#endif

namespace app {

namespace {

std::string Trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const size_t b = s.find_first_not_of(ws);
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

std::string Lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ParseBool(const std::string& v, bool* out) {
    const std::string l = Lower(v);
    if (l == "true" || l == "yes" || l == "on" || l == "1") return *out = true, true;
    if (l == "false" || l == "no" || l == "off" || l == "0") return *out = false, true;
    return false;
}

std::wstring Wide(const std::string& utf8) { return util::u8(utf8).wstring(); }
std::string Utf8(const std::wstring& w) { return util::u8(std::filesystem::path(w)); }

}  // namespace

unsigned cacheAutoMb() {
    uint64_t total = 0;
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (GlobalMemoryStatusEx(&m)) total = m.ullTotalPhys;
#elif defined(__APPLE__)
    uint64_t bytes = 0;
    size_t size = sizeof bytes;
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) total = bytes;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) total = uint64_t(pages) * uint64_t(pageSize);
#endif
    const uint64_t mb = (total >> 20) / 4;
    return unsigned(std::clamp<uint64_t>(mb, 512, 16384));
}

vfs::BuildOptions buildOptions(const Settings& s) {
    vfs::BuildOptions options;
    options.dng.output = s.format;
    options.dng.timecode = s.timecode;
    options.fix = s.rawFixes;
    options.fixes.focusPixels = s.focusPixels;
    options.fixes.badPixels = s.badPixels == 3 ? 3 : s.badPixels ? 1 : 0;  // aggressive: auto, searched harder
    options.fixes.badPixelSearch = s.badPixels == 2 ? 1 : 0;
    options.testPatterns = s.testPatterns;
    return options;
}

std::wstring normalizeDrive(std::wstring drive) {
    if ((drive.size() == 1 || (drive.size() == 2 && drive[1] == L':')) && std::iswalpha(wint_t(drive[0])))
        return std::wstring(1, wchar_t(std::towupper(wint_t(drive[0])))) + L":";
    return drive;
}

std::wstring settingsPath() {
#ifdef _WIN32
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) path = appData;
    CoTaskMemFree(appData);
    if (path.empty()) path = L".";
    return path + L"\\MLV Drive\\mlvdrive.ini";
#elif defined(__APPLE__)
    return Wide(util::homeFolder() + "/Library/Application Support/MLV Drive/mlvdrive.ini");
#else
    return Wide(util::homeFolder() + "/.config/mlvdrive/mlvdrive.ini");
#endif
}

Settings loadSettings(const std::wstring& path, std::vector<std::string>* warnings) {
    Settings s;
    std::FILE* f = util::openFile(Utf8(path), "rb");
    if (!f) return s;  // no file yet: the defaults
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // a UTF-8 BOM

    auto warn = [&](int line, const std::string& what) {
        if (warnings) warnings->push_back("mlvdrive.ini line " + std::to_string(line) + ": " + what);
    };
    size_t at = 0;
    int lineNo = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        ++lineNo;
        // A comment: a line starting with ; or #, or the rest of one from a ; after a space or tab
        // (as this file writes them). A # later in a line is kept: folder names have them
        // ("Shoot #2"), as they may have a ; not after a space.
        line = Trim(line);
        if (!line.empty() && (line[0] == ';' || line[0] == '#')) line.clear();
        for (size_t i = 1; i < line.size(); ++i) {
            if (line[i] == ';' && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
                line.erase(i);
                break;
            }
        }
        line = Trim(line);
        if (line.empty() || line[0] == '[') continue;  // sections only group the keys
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            warn(lineNo, "no '=' in \"" + line + "\"");
            continue;
        }
        const std::string key = Lower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));
        bool ok = true;
        if (key == "folder") {
            if (!value.empty()) s.folders.push_back(Wide(value));
        }
        else if (key == "drive") s.drive = normalizeDrive(Wide(value));
        else if (key == "test_patterns") ok = ParseBool(value, &s.testPatterns);
        else if (key == "mount_at_start") ok = ParseBool(value, &s.mountAtStart);
        else if (key == "watch") ok = ParseBool(value, &s.watch);
        else if (key == "raw_fixes") ok = ParseBool(value, &s.rawFixes);
        else if (key == "format") {
            const std::string v = Lower(value);
            if (v == "16") s.format = dng::Output::Uncompressed16;
            else if (v == "packed") s.format = dng::Output::Uncompressed;
            else ok = false;
        } else if (key == "focus_pixels") {
            const std::string v = Lower(value);
            if (v == "auto") s.focusPixels = -1;
            else if (v == "on") s.focusPixels = 1;
            else if (v == "off") s.focusPixels = 0;
            else ok = false;
        } else if (key == "bad_pixels") {
            const std::string v = Lower(value);
            if (v == "off") s.badPixels = 0;
            else if (v == "auto") s.badPixels = 1;
            else if (v == "aggressive") s.badPixels = 2;
            else if (v == "map") s.badPixels = 3;
            else ok = false;
        } else if (key == "timecode") {
            const std::string v = Lower(value);
            if (v == "time") s.timecode = dng::Timecode::TimeOfDay;
            else if (v == "count") s.timecode = dng::Timecode::FrameCount;
            else ok = false;
        } else if (key == "cache_mb") {
            char* e = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &e, 10);
            if (Lower(value) == "auto") s.cacheMb = 0;
            else if (value.empty() || *e || v < 16 || v > 1u << 20) ok = false;
            else s.cacheMb = unsigned(v);
        } else if (key == "read_ahead") {
            if (Lower(value) == "auto") {
                s.readAhead = -1;
            } else {
                char* e = nullptr;
                const unsigned long v = std::strtoul(value.c_str(), &e, 10);
                if (value.empty() || *e || v > 256) ok = false;
                else s.readAhead = int(v);
            }
        }
        if (!ok) warn(lineNo, "bad value for " + key + ": \"" + value + "\"");
    }
    return s;
}

bool saveSettings(const std::wstring& path, const Settings& s, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const char* nl = "\r\n";  // Notepad-friendly: the file is for people to edit
    std::string t;
    // key = value, its comment (if any) lined up in one column.
    auto line = [&](const std::string& key, const std::string& value, const char* comment = nullptr) {
        std::string l = key + " = " + value;
        if (comment) {
            if (l.size() < 27) l.append(27 - l.size(), ' ');
            l += std::string(" ; ") + comment;
        }
        t += l + nl;
    };
    t += std::string("; MLV Drive settings (UTF-8). The tray app writes this file; the console program") + nl;
    t += std::string("; takes it as its defaults.") + nl;
    t += std::string("[mount]") + nl;
    if (s.folders.empty()) line("folder", "", "an MLV folder; one line each, merged on the drive");
    for (size_t i = 0; i < s.folders.size(); ++i)
        line("folder", Utf8(s.folders[i]), i == 0 ? "an MLV folder; one line each, merged on the drive" : nullptr);
    line("drive", Utf8(s.drive), "empty: the next free letter from Z: down");
    line("test_patterns", s.testPatterns ? "true" : "false");
    line("mount_at_start", s.mountAtStart ? "true" : "false", "the tray app mounts when it starts");
    line("watch", s.watch ? "true" : "false", "show clips added or changed in the folders as it happens");
    t += std::string("[dng]") + nl;
    line("format", s.format == dng::Output::Uncompressed ? "packed" : "16", "16 or packed");
    line("raw_fixes", s.rawFixes ? "true" : "false", "raw corrections at all");
    line("focus_pixels", s.focusPixels < 0 ? "auto" : s.focusPixels ? "on" : "off",
         "auto (cameras with them, clips with their own .fpm), on, off");
    static const char* const bad[] = {"off", "auto", "aggressive", "map"};
    line("bad_pixels", bad[std::clamp(s.badPixels, 0, 3)], "off, auto, aggressive, map (.bpm)");
    line("timecode", s.timecode == dng::Timecode::FrameCount ? "count" : "time",
         "time (of day) or count (from 00:00:00:00)");
    t += std::string("[performance]") + nl;
    line("cache_mb", s.cacheMb ? std::to_string(s.cacheMb) : std::string("auto"),
         "finished frames kept: auto (a quarter of the RAM) or MB");
    line("read_ahead", s.readAhead < 0 ? std::string("auto") : std::to_string(s.readAhead),
         "auto (one per CPU) or a number of frames");
    return util::writeFile(Utf8(path), t.data(), t.size(), nullptr, 0, error);
}

}  // namespace app
