// mlvdrive_tray: MLV Drive in the notification area (no console). Its menu mounts and unmounts,
// opens the drive, chooses the MLV folder, the drive letter and the options, and turns on starting
// with Windows; the settings are the same file the console program reads (app/settings.h).
//
// Mounting runs on a worker thread (opening many clips takes a moment), so the menu never freezes;
// a settings change while mounted mounts again with it. `mlvdrive unmount M:` unmounts it too.
#include "app/mounter.h"
#include "app/settings.h"
#include "fs/winfsp_fs.h"
#include "parallel.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;   // the icon's mouse events
constexpr UINT kJobDone = WM_APP + 2;       // a mount or unmount finished (lParam: JobResult*)
constexpr UINT kStopRequest = WM_APP + 3;   // `mlvdrive unmount` asked
constexpr UINT kRescanned = WM_APP + 4;     // a rescan finished (lParam: app::RescanResult*)
constexpr UINT kIconId = 1;
const wchar_t kAppName[] = L"MLV Drive";
const wchar_t kVersion[] = L"" MLVDRIVE_VERSION;
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum MenuId : UINT {
    kMount = 100, kUnmount, kOpen, kAddFolder, kDriveAuto, kFormat16, kFormatPacked, kFixes, kTimecodeTime,
    kTimecodeCount, kPatterns, kMountAtStart, kAutostart, kEditSettings, kReloadSettings, kGetWinFsp, kQuit,
    kFocusAuto, kFocusOn, kFocusOff, kBadOff, kBadAuto, kBadAggressive, kBadMap, kRescan, kWatch,
    kDriveFirst = 200,         // + letter index (A = 0)
    kRemoveFolderFirst = 300   // + the folder's index
};

struct JobResult {
    bool mount = false;   // a mount (else an unmount)
    bool ok = false;
    bool notify = false;  // tell the user it worked (they asked for it)
    std::string error;
    app::MountInfo info;
    std::vector<std::string> skipped;  // clips that couldn't be opened
};

HINSTANCE g_inst;
HWND g_wnd;
UINT g_taskbarCreated;
HICON g_iconOn, g_iconOff;
std::wstring g_settingsPath;
app::Settings g_settings;
bool g_winfsp = false;

enum class State { Unmounted, Busy, Mounted } g_state = State::Unmounted;
std::wstring g_busyText;
app::MountInfo g_info;
std::string g_lastError;

app::Mounter g_mounter;  // used by one worker thread at a time (and at exit)
std::thread g_worker;
HANDLE g_stopEvent = nullptr, g_stopWait = nullptr;

// --- The icon ---

void UpdateIcon(DWORD message = NIM_MODIFY) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_wnd;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = g_state == State::Mounted ? g_iconOn : g_iconOff;
    std::wstring tip = std::wstring(kAppName) + L" " + kVersion;
    if (g_state == State::Mounted)
        tip += L" — " + g_info.mountPoint + L" (" + std::to_wstring(g_info.clips) +
               (g_info.clips == 1 ? L" clip)" : L" clips)");
    else if (g_state == State::Busy) tip += L" — " + g_busyText;
    else tip += L" — not mounted";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(message, &nid);
    if (message == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void Balloon(const std::wstring& title, const std::wstring& text, bool error) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_wnd;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// --- Mounting (on a worker thread) ---

void StopWatching() {
    if (g_stopWait) UnregisterWaitEx(g_stopWait, INVALID_HANDLE_VALUE);
    g_stopWait = nullptr;
    if (g_stopEvent) CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
}

void CALLBACK OnStopEvent(PVOID, BOOLEAN) {
    PostMessageW(g_wnd, kStopRequest, 0, 0);
}

// `mlvdrive unmount M:` sets the mount's named event (as for a console mount).
void WatchStopEvent(const std::wstring& mountPoint) {
    StopWatching();
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, app::stopEventName(mountPoint).c_str());
    if (g_stopEvent)
        RegisterWaitForSingleObject(&g_stopWait, g_stopEvent, OnStopEvent, nullptr, INFINITE, WT_EXECUTEONLYONCE);
}

void StartJob(bool mount, bool notify) {
    if (g_state == State::Busy) return;
    if (g_worker.joinable()) g_worker.join();
    StopWatching();
    g_state = State::Busy;
    g_busyText = mount ? L"mounting…" : L"unmounting…";
    UpdateIcon();
    const app::Settings settings = g_settings;
    g_worker = std::thread([mount, notify, settings] {
        auto r = std::make_unique<JobResult>();
        r->mount = mount;
        r->notify = notify;
        if (mount) {
            std::vector<std::string> skipped;
            r->ok = g_mounter.mount(settings, false, [&](const std::string& m) { skipped.push_back(m); }, &r->error);
            r->skipped = std::move(skipped);
            if (r->ok) r->info = g_mounter.info();
        } else {
            g_mounter.unmount();
            r->ok = true;
        }
        PostMessageW(g_wnd, kJobDone, 0, reinterpret_cast<LPARAM>(r.release()));
    });
}

// Looks at the folders again (on a worker thread): what changed shows without unmounting.
void StartRescan() {
    if (g_state != State::Mounted) return;
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread([] {
        auto r = std::make_unique<app::RescanResult>();
        std::string error;
        if (g_mounter.rescan(r.get(), &error)) PostMessageW(g_wnd, kRescanned, 0, reinterpret_cast<LPARAM>(r.release()));
    });
}

// After a rescan (asked for in the menu, or by the folders changing): the counts, and a word when
// clips came or went.
void Rescanned(app::RescanResult* raw) {
    std::unique_ptr<app::RescanResult> r(raw);
    if (g_state != State::Mounted) return;
    g_info = r->info;
    UpdateIcon();
    if (r->added || r->removed || r->changed) {
        std::wstring text;
        auto part = [&](size_t n, const wchar_t* what) {
            if (!n) return;
            if (!text.empty()) text += L", ";
            text += std::to_wstring(n) + (n == 1 ? L" clip " : L" clips ") + what;
        };
        part(r->added, L"added");
        part(r->removed, L"removed");
        part(r->changed, L"changed");
        Balloon(g_info.mountPoint + L" updated", text + L".", false);
    } else if (!r->automatic) {
        Balloon(g_info.mountPoint + L" is up to date", L"No clips added, removed or changed.", false);
    }
}

void JobDone(JobResult* raw) {
    std::unique_ptr<JobResult> r(raw);
    if (r->mount && r->ok) {
        g_state = State::Mounted;
        g_info = r->info;
        g_lastError.clear();
        WatchStopEvent(g_info.mountPoint);
        UpdateIcon();
        if (!r->skipped.empty()) {
            Balloon(L"Some clips were left out",
                    std::to_wstring(r->skipped.size()) + L" couldn't be opened, e.g.:\n" + app::widen(r->skipped[0]), true);
        } else if (r->notify) {
            wchar_t text[256];
            swprintf_s(text, L"%zu clip%s, %.1f GB of DNG.", g_info.clips, g_info.clips == 1 ? L"" : L"s",
                       g_info.bytes / 1e9);
            Balloon(L"Mounted on " + g_info.mountPoint, text, false);
        }
    } else {
        g_state = State::Unmounted;
        if (r->mount) {
            g_lastError = r->error;
            UpdateIcon();
            Balloon(L"Couldn't mount", app::widen(r->error), true);
        } else {
            UpdateIcon();
        }
    }
}

// Saves the settings; if mounted, mounts again with them.
void SettingsChanged() {
    std::string error;
    if (!app::saveSettings(g_settingsPath, g_settings, &error))
        Balloon(L"Couldn't save the settings", app::widen(error), true);
    if (g_state == State::Mounted) StartJob(true, false);
}

// --- Menu actions ---

// Asks for a folder to add; true if one was added.
bool AddFolder() {
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return false;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"MLV Drive: the folder of MLV clips (subfolders included)");
    if (!g_settings.folders.empty()) {
        IShellItem* start = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_settings.folders.back().c_str(), nullptr, IID_PPV_ARGS(&start)))) {
            dialog->SetFolder(start);
            start->Release();
        }
    }
    bool chosen = false;
    if (SUCCEEDED(dialog->Show(g_wnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                const std::wstring folder = path;
                CoTaskMemFree(path);
                bool known = false;
                for (const auto& f : g_settings.folders)
                    known = known || CompareStringOrdinal(f.c_str(), -1, folder.c_str(), -1, TRUE) == CSTR_EQUAL;
                if (!known) {
                    g_settings.folders.push_back(folder);
                    chosen = true;
                }
            }
            item->Release();
        }
    }
    dialog->Release();
    return chosen;
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    return std::wstring(path, n);
}

bool Autostart() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        const std::wstring command = L"\"" + ExePath() + L"\"";
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

std::wstring Shorten(const std::wstring& path, size_t max) {
    return path.size() <= max ? path : L"…" + path.substr(path.size() - (max - 1));
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    const bool busy = g_state == State::Busy, mounted = g_state == State::Mounted;
    auto item = [](HMENU m, UINT id, const std::wstring& text, bool enabled = true, bool checked = false) {
        AppendMenuW(m, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), id, text.c_str());
    };
    auto radio = [&](HMENU m, UINT id, const std::wstring& text, bool checked) {
        item(m, id, text);
        if (checked) CheckMenuRadioItem(m, id, id, id, MF_BYCOMMAND);
    };

    // The state, then what can be done about it.
    std::wstring status;
    if (!g_winfsp) status = L"WinFsp isn't installed";
    else if (mounted) {
        wchar_t text[128];
        swprintf_s(text, L"Mounted on %s — %zu clip%s, %.1f GB", g_info.mountPoint.c_str(), g_info.clips,
                   g_info.clips == 1 ? L"" : L"s", g_info.bytes / 1e9);
        status = text;
    } else if (busy) status = L"MLV Drive — " + g_busyText;
    else if (!g_lastError.empty()) status = L"Not mounted: " + Shorten(app::widen(g_lastError), 60);
    else status = L"MLV Drive — not mounted";
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (!g_winfsp) {
        item(menu, kGetWinFsp, L"Get WinFsp (needed for the drive)…");
    } else if (mounted) {
        item(menu, kOpen, L"Open " + g_info.mountPoint + L" in Explorer");
        if (!g_settings.watch) item(menu, kRescan, L"Rescan the folders", !busy);  // watched: not needed
        item(menu, kUnmount, L"Unmount " + g_info.mountPoint, !busy);
    } else {
        const bool canMount = !busy && (!g_settings.folders.empty() || g_settings.testPatterns);
        const std::wstring where = g_settings.drive.empty() ? L"the next free letter" : g_settings.drive;
        item(menu, kMount, L"Mount on " + where, canMount);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (g_settings.folders.empty()) {
        item(menu, kAddFolder, L"Choose an MLV folder…", !busy);
    } else {
        HMENU folders = CreatePopupMenu();
        for (size_t i = 0; i < g_settings.folders.size() && i < 50; ++i)
            item(folders, kRemoveFolderFirst + UINT(i), L"Remove " + Shorten(g_settings.folders[i], 60));
        AppendMenuW(folders, MF_SEPARATOR, 0, nullptr);
        item(folders, kAddFolder, L"Add a folder…");
        const size_t n = g_settings.folders.size();
        const std::wstring label = n == 1 ? L"MLV folder: " + Shorten(g_settings.folders[0], 48)
                                          : L"MLV folders: " + std::to_wstring(n);
        AppendMenuW(menu, MF_POPUP | (busy ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(folders), label.c_str());
    }

    HMENU drives = CreatePopupMenu();
    radio(drives, kDriveAuto, L"Next free letter (from Z: down)", g_settings.drive.empty());
    AppendMenuW(drives, MF_SEPARATOR, 0, nullptr);
    const DWORD used = GetLogicalDrives();
    const wchar_t ours = mounted && g_info.mountPoint.size() == 2 ? g_info.mountPoint[0] : 0;
    for (int i = 3; i < 26; ++i) {  // D: to Z:
        const wchar_t letter = wchar_t(L'A' + i);
        const bool free = !(used & (1u << i)) || letter == ours;
        const std::wstring name = std::wstring(1, letter) + L":";
        radio(drives, kDriveFirst + UINT(i), free ? name : name + L"  (in use)", g_settings.drive == name);
        if (!free) EnableMenuItem(drives, kDriveFirst + UINT(i), MF_BYCOMMAND | MF_GRAYED);
    }
    AppendMenuW(menu, MF_POPUP | (busy ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(drives),
                (L"Drive letter: " + (g_settings.drive.empty() ? std::wstring(L"next free") : g_settings.drive)).c_str());

    HMENU dng = CreatePopupMenu();
    radio(dng, kFormat16, L"16-bit uncompressed", g_settings.format == dng::Output::Uncompressed16);
    radio(dng, kFormatPacked, L"Uncompressed, packed to the clip's bit depth", g_settings.format == dng::Output::Uncompressed);
    AppendMenuW(dng, MF_SEPARATOR, 0, nullptr);
    item(dng, kFixes, L"Raw corrections (stripes, focus and bad pixels)", true, g_settings.rawFixes);
    HMENU focus = CreatePopupMenu();
    radio(focus, kFocusAuto, L"Auto (cameras with focus pixels, clips with their own .fpm)", g_settings.focusPixels < 0);
    radio(focus, kFocusOn, L"On", g_settings.focusPixels > 0);
    radio(focus, kFocusOff, L"Off", g_settings.focusPixels == 0);
    AppendMenuW(dng, MF_POPUP | (g_settings.rawFixes ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(focus), L"Focus pixels");
    HMENU bad = CreatePopupMenu();
    radio(bad, kBadOff, L"Off", g_settings.badPixels == 0);
    radio(bad, kBadAuto, L"Auto (searched on the first frame)", g_settings.badPixels == 1);
    radio(bad, kBadAggressive, L"Aggressive (searched harder)", g_settings.badPixels == 2);
    radio(bad, kBadMap, L"From a map (.bpm)", g_settings.badPixels == 3);
    AppendMenuW(dng, MF_POPUP | (g_settings.rawFixes ? 0 : MF_GRAYED), reinterpret_cast<UINT_PTR>(bad), L"Bad pixels");
    AppendMenuW(dng, MF_SEPARATOR, 0, nullptr);
    radio(dng, kTimecodeTime, L"Timecode: time of day", g_settings.timecode == dng::Timecode::TimeOfDay);
    radio(dng, kTimecodeCount, L"Timecode: from 00:00:00:00", g_settings.timecode == dng::Timecode::FrameCount);
    AppendMenuW(menu, MF_POPUP | (busy ? MF_GRAYED : 0), reinterpret_cast<UINT_PTR>(dng), L"DNG");
    item(menu, kPatterns, L"Test patterns (12 charts, 5 s each)", !busy, g_settings.testPatterns);
    item(menu, kWatch, L"Watch the folders for new clips", !busy, g_settings.watch);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    item(menu, kMountAtStart, L"Mount when MLV Drive starts", true, g_settings.mountAtStart);
    item(menu, kAutostart, L"Start with Windows", true, Autostart());
    item(menu, kEditSettings, L"Edit the settings file…");
    item(menu, kReloadSettings, L"Reload the settings file", !busy);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, (std::wstring(kAppName) + L" " + kVersion).c_str());
    item(menu, kQuit, mounted ? L"Quit (unmounts " + g_info.mountPoint + L")" : L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);  // so the menu closes when clicking elsewhere
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // and its submenus
}

void OpenDrive() {
    if (g_state == State::Mounted) ShellExecuteW(nullptr, L"open", (g_info.mountPoint + L"\\").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Quit() {
    if (g_worker.joinable()) g_worker.join();
    StopWatching();
    g_mounter.unmount();
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_wnd;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(g_wnd);
}

void Command(UINT id) {
    switch (id) {
        case kMount: StartJob(true, true); return;
        case kUnmount: StartJob(false, true); return;
        case kOpen: OpenDrive(); return;
        case kGetWinFsp: ShellExecuteW(nullptr, L"open", L"https://winfsp.dev/rel/", nullptr, nullptr, SW_SHOWNORMAL); return;
        case kAddFolder:
            if (AddFolder()) {
                SettingsChanged();
                if (g_state == State::Unmounted && g_winfsp) StartJob(true, true);
            }
            return;
        case kDriveAuto: g_settings.drive.clear(); SettingsChanged(); return;
        case kFormat16: g_settings.format = dng::Output::Uncompressed16; SettingsChanged(); return;
        case kFormatPacked: g_settings.format = dng::Output::Uncompressed; SettingsChanged(); return;
        case kFixes: g_settings.rawFixes = !g_settings.rawFixes; SettingsChanged(); return;
        case kFocusAuto: g_settings.focusPixels = -1; SettingsChanged(); return;
        case kFocusOn: g_settings.focusPixels = 1; SettingsChanged(); return;
        case kFocusOff: g_settings.focusPixels = 0; SettingsChanged(); return;
        case kBadOff: g_settings.badPixels = 0; SettingsChanged(); return;
        case kBadAuto: g_settings.badPixels = 1; SettingsChanged(); return;
        case kBadAggressive: g_settings.badPixels = 2; SettingsChanged(); return;
        case kBadMap: g_settings.badPixels = 3; SettingsChanged(); return;
        case kTimecodeTime: g_settings.timecode = dng::Timecode::TimeOfDay; SettingsChanged(); return;
        case kTimecodeCount: g_settings.timecode = dng::Timecode::FrameCount; SettingsChanged(); return;
        case kPatterns: g_settings.testPatterns = !g_settings.testPatterns; SettingsChanged(); return;
        case kMountAtStart: g_settings.mountAtStart = !g_settings.mountAtStart; SettingsChanged(); return;
        case kRescan: StartRescan(); return;
        case kWatch: g_settings.watch = !g_settings.watch; SettingsChanged(); return;
        case kAutostart: SetAutostart(!Autostart()); return;
        case kEditSettings: {
            std::string error;
            if (GetFileAttributesW(g_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                app::saveSettings(g_settingsPath, g_settings, &error);
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_settingsPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        case kReloadSettings: {
            std::vector<std::string> warnings;
            g_settings = app::loadSettings(g_settingsPath, &warnings);
            if (!warnings.empty()) Balloon(L"Settings file", app::widen(warnings[0]), true);
            if (g_state == State::Mounted) StartJob(true, false);
            return;
        }
        case kQuit: Quit(); return;
        default:
            if (id >= kDriveFirst && id < kDriveFirst + 26) {
                g_settings.drive = std::wstring(1, wchar_t(L'A' + (id - kDriveFirst))) + L":";
                SettingsChanged();
            } else if (id >= kRemoveFolderFirst && id < kRemoveFolderFirst + g_settings.folders.size()) {
                g_settings.folders.erase(g_settings.folders.begin() + (id - kRemoveFolderFirst));
                SettingsChanged();
                // Nothing left to show: unmount.
                if (g_settings.folders.empty() && !g_settings.testPatterns && g_state != State::Unmounted)
                    StartJob(false, false);
            }
    }
}

LRESULT CALLBACK WindowProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) {  // Explorer restarted: the icon again
        UpdateIcon(NIM_ADD);
        return 0;
    }
    switch (msg) {
        case kTrayMessage:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); return 0;
                case WM_LBUTTONDBLCLK: OpenDrive(); return 0;
            }
            return 0;
        case kJobDone: JobDone(reinterpret_cast<JobResult*>(lp)); return 0;
        case kRescanned: Rescanned(reinterpret_cast<app::RescanResult*>(lp)); return 0;
        case kStopRequest:
            if (g_state == State::Mounted) StartJob(false, false);
            return 0;
        case WM_COMMAND: Command(LOWORD(wp)); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            if (wp) Quit();
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    // One at a time.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MLVDrive-tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"MLV Drive is already running: its icon is in the notification area.", kAppName,
                    MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Rescans started by the folders changing (or `mlvdrive rescan`) come from the mounter's thread.
    g_mounter.onRescan([](const app::RescanResult& r) {
        PostMessageW(g_wnd, kRescanned, 0, reinterpret_cast<LPARAM>(new app::RescanResult(r)));
    });
    g_inst = inst;
    LoadIconMetric(inst, MAKEINTRESOURCEW(1), LIM_SMALL, &g_iconOn);
    LoadIconMetric(inst, MAKEINTRESOURCEW(2), LIM_SMALL, &g_iconOff);

    g_settingsPath = app::settingsPath();
    std::vector<std::string> warnings;
    g_settings = app::loadSettings(g_settingsPath, &warnings);
    const bool firstRun = GetFileAttributesW(g_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES;
    std::string error;
    g_winfsp = fsys::Drive::loadWinFsp(&error);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = inst;
    wc.hIcon = g_iconOn;
    wc.lpszClassName = L"MLVDriveTray";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateIcon(NIM_ADD);

    if (!warnings.empty()) Balloon(L"Settings file", app::widen(warnings[0]), true);
    if (!g_winfsp) {
        Balloon(L"WinFsp isn't installed", L"MLV Drive needs it for the drive: right-click the icon to get it.", true);
    } else if (firstRun) {
        app::saveSettings(g_settingsPath, g_settings, &error);
        Balloon(kAppName, L"Right-click the icon to choose your MLV folder.", false);
    } else if (g_settings.mountAtStart && (!g_settings.folders.empty() || g_settings.testPatterns)) {
        StartJob(true, false);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_worker.joinable()) g_worker.join();
    g_mounter.unmount();
    par::shutdown();
    CoUninitialize();
    if (single) CloseHandle(single);
    return 0;
}
