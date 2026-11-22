// MLV Drive.app: the menu bar app (the Windows tray app's counterpart). It carries the FSKit module
// (Contents/Extensions/MLVDriveFS.appex), where the drive runs, and the `mlvdrive` command; it
// mounts and unmounts through the system (mount -F -t mlvdrive / umount), with MLV Drive's
// settings folder as the source: the module then shows the settings' folders with the settings'
// options. A setting changed while mounted mounts again with it.
//
// Its icon: a drive, filled while mounted (a template image: it follows the menu bar's colour).
// The state is read from the system (getmntinfo) every 3 seconds and when the menu opens, so an
// eject in Finder or a `umount` shows.
//
// Setup, once (the menu's Setup section says what is missing and opens the right pane): the file
// system extension turned on (System Settings > General > Login Items & Extensions > File System
// Extensions), and Full Disk Access for this app, which the module's reads are checked against
// (clips on the Desktop, in Documents, on external or network drives).
#import <AppKit/AppKit.h>
#import <ServiceManagement/ServiceManagement.h>

#include "app/settings.h"
#include "util/file.h"

#include <notify.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

NSString* const kAppName = @"MLV Drive";
const char kModuleId[] = "org.mlvdrive.MLVDrive.fs";

NSString* toNS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }
std::wstring toW(NSString* s) { return util::widen(s.UTF8String ?: ""); }

// ~ and ~/... -> the home folder.
std::string expandHome(const std::string& path) {
    if (path == "~") return util::homeFolder();
    if (path.rfind("~/", 0) == 0) return util::homeFolder() + path.substr(1);
    return path;
}

// The home folder as ~ again (shorter in the menu).
NSString* displayPath(const std::string& path) {
    const std::string home = util::homeFolder();
    if (path.rfind(home + "/", 0) == 0) return toNS("~" + path.substr(home.size()));
    return toNS(path);
}

NSString* shorten(NSString* s, NSUInteger max) {
    return s.length <= max ? s : [@"…" stringByAppendingString:[s substringFromIndex:s.length - (max - 1)]];
}

std::string settingsFolder() {
    return util::u8(util::u8(util::narrow(app::settingsPath())).parent_path());
}

// The MLV Drive volume mounted at `mountPoint`, if any.
bool findMount(const std::string& mountPoint, struct statfs* out) {
    char real[PATH_MAX];
    const std::string want = realpath(mountPoint.c_str(), real) ? real : mountPoint;
    struct statfs* mounts = nullptr;
    const int n = getmntinfo(&mounts, MNT_NOWAIT);
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(mounts[i].f_fstypename, "mlvdrive") == 0 && want == mounts[i].f_mntonname) {
            if (out) *out = mounts[i];
            return true;
        }
    }
    return false;
}

// Whether FSKit has the module turned on (its own list; private, so "unknown" if it can't be read).
enum class Tri { No, Yes, Unknown };
Tri moduleEnabled() {
    NSString* path = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Group Containers/group.com.apple.fskit.settings/enabledModules.plist"];
    NSArray* list = [NSArray arrayWithContentsOfFile:path];
    if (!list) return Tri::Unknown;
    return [list containsObject:@(kModuleId)] ? Tri::Yes : Tri::No;
}

// Full Disk Access: only then can a process read the privacy database.
bool fullDiskAccess() {
    const std::string db = util::homeFolder() + "/Library/Application Support/com.apple.TCC/TCC.db";
    FILE* f = std::fopen(db.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// Runs a program; its exit code, its error output in `err`.
int runTool(NSString* path, NSArray<NSString*>* args, NSString** err) {
    NSTask* task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:path];
    task.arguments = args;
    NSPipe* pipe = [NSPipe pipe];
    task.standardError = pipe;
    task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    NSError* error = nil;
    if (![task launchAndReturnError:&error]) {
        if (err) *err = error.localizedDescription;
        return -1;
    }
    NSData* data = [pipe.fileHandleForReading readDataToEndOfFile];
    [task waitUntilExit];
    if (err) {
        NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
        *err = [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    }
    return task.terminationStatus;
}

// The menu bar icon: a drive, filled while mounted.
NSImage* driveIcon(bool mounted) {
    NSImage* image = [NSImage imageWithSize:NSMakeSize(18, 18)
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect) {
                                 [NSColor.blackColor set];
                                 NSBezierPath* body = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(1.5, 4.5, 15, 9)
                                                                                      xRadius:2
                                                                                      yRadius:2];
                                 body.lineWidth = 1.4;
                                 if (mounted) {
                                     [body fill];
                                     [[NSColor colorWithWhite:0 alpha:0] set];
                                     [NSGraphicsContext.currentContext setCompositingOperation:NSCompositingOperationClear];
                                     NSRectFill(NSMakeRect(11.5, 7.5, 2.5, 2.5));  // the light, cut out
                                 } else {
                                     [body stroke];
                                     NSRectFill(NSMakeRect(11.5, 7.5, 2.5, 2.5));
                                 }
                                 return YES;
                             }];
    [image setTemplate:YES];  // (`template` is a C++ keyword)
    return image;
}

using Action = void (^)(void);

// A menu action, or none (the item greyed).
Action when(bool enabled, Action action) { return enabled ? action : (Action)nil; }

}  // namespace

// --- the app ---

@interface MLVMenuBar : NSObject <NSApplicationDelegate, NSMenuDelegate>
@end

@implementation MLVMenuBar {
    NSStatusItem* _item;
    app::Settings _settings;
    std::vector<std::string> _warnings;
    bool _busy;
    NSString* _busyText;
    NSString* _lastError;
    bool _mounted;
    std::string _mountedAt;  // where it is mounted (the settings may name another place since)
    NSTimer* _timer;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    // One at a time.
    for (NSRunningApplication* other in
         [NSRunningApplication runningApplicationsWithBundleIdentifier:NSBundle.mainBundle.bundleIdentifier]) {
        if (other.processIdentifier != getpid()) {
            [NSApp terminate:nil];
            return;
        }
    }
    [self loadSettings];
    _item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    _item.button.toolTip = [NSString stringWithFormat:@"%@ %s", kAppName, MLVDRIVE_VERSION];
    NSMenu* menu = [[NSMenu alloc] init];
    menu.delegate = self;
    menu.autoenablesItems = NO;
    _item.menu = menu;
    [self refreshState];
    _timer = [NSTimer scheduledTimerWithTimeInterval:3 repeats:YES block:^(NSTimer*) { [self refreshState]; }];
    if (_settings.mountAtStart && !_mounted && [self canMount]) [self mount];
    else if (_settings.folders.empty() && !_settings.testPatterns) [self welcome];
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    // As on Windows: quitting unmounts (synchronously: the app is going).
    if (_mounted) runTool(@"/sbin/umount", @[ toNS(_mountedAt) ], nil);
}

// --- settings ---

- (void)loadSettings {
    _warnings.clear();
    _settings = app::loadSettings(app::settingsPath(), &_warnings);
}

- (bool)saveSettings {
    std::error_code ec;
    std::filesystem::create_directories(util::u8(settingsFolder()), ec);
    std::string error;
    if (!app::saveSettings(app::settingsPath(), _settings, &error)) {
        [self alert:@"Can't save the settings" text:toNS(error)];
        return false;
    }
    return true;
}

- (std::string)mountPoint {
    std::string at = expandHome(util::narrow(_settings.drive));
    if (at.empty()) at = util::homeFolder() + "/MLV Drive";
    return at;
}

- (bool)canMount {
    return !_busy && (!_settings.folders.empty() || _settings.testPatterns);
}

// A setting changed: saved, and the drive mounted again with it.
- (void)changed {
    if (![self saveSettings]) return;
    if (_mounted) [self remount];
}

// --- state ---

- (void)refreshState {
    if (!_busy) {
        const std::string at = _mounted ? _mountedAt : [self mountPoint];
        _mounted = findMount(at, nullptr);
        if (_mounted) _mountedAt = at;
    }
    _item.button.image = driveIcon(_mounted);
}

- (NSString*)statusText {
    if (_busy) return [NSString stringWithFormat:@"%@ — %@", kAppName, _busyText];
    struct statfs s;
    if (_mounted && findMount(_mountedAt, &s)) {
        // The module's statistics: files = clip folders + 2, blocks = the DNGs' size.
        const uint64_t clips = s.f_files >= 2 ? s.f_files - 2 : 0;
        const double gb = double(s.f_blocks) * double(s.f_bsize) / 1e9;
        return [NSString stringWithFormat:@"Mounted on %@ — %llu clip%@, %.1f GB", displayPath(_mountedAt),
                                          (unsigned long long)clips, clips == 1 ? @"" : @"s", gb];
    }
    if (_lastError.length) return [@"Not mounted: " stringByAppendingString:shorten(_lastError, 70)];
    return [NSString stringWithFormat:@"%@ — not mounted", kAppName];
}

// --- mounting ---

- (void)setBusy:(NSString*)text {
    _busy = text != nil;
    _busyText = text;
    [self refreshState];
}

- (void)mount {
    if (![self canMount] || ![self saveSettings]) return;
    const std::string at = [self mountPoint];
    std::error_code ec;
    std::filesystem::create_directories(util::u8(at), ec);
    [self setBusy:@"mounting…"];
    _lastError = nil;
    NSString* source = toNS(settingsFolder());
    NSString* target = toNS(at);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* err = nil;
        const int code = runTool(@"/sbin/mount", @[ @"-F", @"-t", @"mlvdrive", source, target ], &err);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setBusy:nil];
            if (code == 0 && findMount(at, nullptr)) {
                self->_mounted = true;
                self->_mountedAt = at;
                [self refreshState];
                return;
            }
            [self mountFailed:err];
        });
    });
}

- (void)mountFailed:(NSString*)err {
    if ([err containsString:@"disabled"] || moduleEnabled() == Tri::No) {
        _lastError = @"the file system extension is turned off";
        [self explainSetup];
        return;
    }
    _lastError = err.length ? err : @"mount failed";
    [self alert:@"MLV Drive couldn't mount"
           text:[NSString stringWithFormat:@"%@\n\nAre there MLV clips in the folders, and can MLV Drive read them? Clips "
                                           @"on the Desktop, in Documents or on external or network drives need Full "
                                           @"Disk Access for MLV Drive (Setup in the menu).",
                                           _lastError]];
}

// Unmounts; `then` runs after (with whether it worked). Busy (a program has a file open): asks
// whether to force it.
- (void)unmountThen:(void (^)(bool))then {
    if (!_mounted) {
        if (then) then(true);
        return;
    }
    [self setBusy:@"unmounting…"];
    NSString* target = toNS(_mountedAt);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSString* err = nil;
        const int code = runTool(@"/sbin/umount", @[ target ], &err);
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setBusy:nil];
            if (code == 0) {
                self->_mounted = false;
                [self refreshState];
                if (then) then(true);
                return;
            }
            NSAlert* a = [[NSAlert alloc] init];
            a.messageText = @"MLV Drive is in use";
            a.informativeText = [NSString stringWithFormat:@"A program has a file on %@ open (%@). Unmount it anyway? "
                                                           @"The program loses the file.",
                                                           target, err ?: @""];
            [a addButtonWithTitle:@"Cancel"];
            [a addButtonWithTitle:@"Unmount Anyway"];
            [NSApp activateIgnoringOtherApps:YES];
            if ([a runModal] == NSAlertSecondButtonReturn) {
                runTool(@"/sbin/umount", @[ @"-f", target ], nil);
                self->_mounted = findMount(util::narrow(toW(target)), nullptr);
            }
            [self refreshState];
            if (then) then(!self->_mounted);
        });
    });
}

- (void)remount {
    [self unmountThen:^(bool ok) {
        if (ok) [self mount];
    }];
}

// --- messages ---

- (void)alert:(NSString*)title text:(NSString*)text {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = title;
    a.informativeText = text ?: @"";
    [NSApp activateIgnoringOtherApps:YES];
    [a runModal];
}

- (void)welcome {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"MLV Drive";
    a.informativeText =
        @"MLV Drive shows Magic Lantern MLV clips as folders of CinemaDNG files, made as they are read, so DaVinci "
        @"Resolve imports MLV directly.\n\nChoose a folder of MLV clips to start. MLV Drive lives in the menu bar.";
    [a addButtonWithTitle:@"Choose a Folder…"];
    [a addButtonWithTitle:@"Later"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([a runModal] == NSAlertFirstButtonReturn) [self addFolder];
}

- (void)explainSetup {
    NSAlert* a = [[NSAlert alloc] init];
    a.messageText = @"Turn on MLV Drive's file system extension";
    a.informativeText =
        @"Once: in System Settings > General > Login Items & Extensions > File System Extensions, turn on "
        @"\"MLV Drive\" (quit and reopen System Settings if it isn't listed). Then mount again.";
    [a addButtonWithTitle:@"Open System Settings"];
    [a addButtonWithTitle:@"Cancel"];
    [NSApp activateIgnoringOtherApps:YES];
    if ([a runModal] == NSAlertFirstButtonReturn) [self openExtensionsSettings];
}

- (void)openExtensionsSettings {
    [NSWorkspace.sharedWorkspace
        openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.LoginItems-Settings.extension"]];
}

- (void)openFullDiskAccess {
    [NSWorkspace.sharedWorkspace
        openURL:[NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"]];
}

// --- actions ---

- (void)addFolder {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = YES;
    panel.prompt = @"Add";
    panel.message = @"MLV folders: their clips (in subfolders too) show on the drive, merged.";
    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK) return;
    bool added = false;
    for (NSURL* url in panel.URLs) {
        const std::wstring f = toW(url.path);
        if (std::find(_settings.folders.begin(), _settings.folders.end(), f) == _settings.folders.end()) {
            _settings.folders.push_back(f);
            added = true;
        }
    }
    if (!added) return;
    if (_mounted) [self changed];
    else if ([self saveSettings] && _settings.mountAtStart) [self mount];
}

- (void)chooseMountPoint {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES;
    panel.prompt = @"Use";
    panel.message = @"An empty folder where the drive shows (not in /Volumes: macOS keeps that for itself).";
    [NSApp activateIgnoringOtherApps:YES];
    if ([panel runModal] != NSModalResponseOK) return;
    _settings.drive = toW(panel.URL.path);
    [self changed];
}

- (void)editSettings {
    if (![self saveSettings]) return;  // (so the file exists)
    [NSWorkspace.sharedWorkspace openURLs:@[ [NSURL fileURLWithPath:toNS(util::narrow(app::settingsPath()))] ]
                     withApplicationAtURL:[NSURL fileURLWithPath:@"/System/Applications/TextEdit.app"]
                            configuration:[NSWorkspaceOpenConfiguration configuration]
                        completionHandler:nil];
}

- (void)reloadSettings {
    [self loadSettings];
    if (!_warnings.empty()) {
        std::string text;
        for (const auto& w : _warnings) text += w + "\n";
        [self alert:@"Some settings were left at their defaults" text:toNS(text)];
    }
    if (_mounted) [self remount];
}

- (bool)startsAtLogin {
    return SMAppService.mainAppService.status == SMAppServiceStatusEnabled;
}

- (void)toggleStartAtLogin {
    NSError* error = nil;
    if ([self startsAtLogin]) [SMAppService.mainAppService unregisterAndReturnError:&error];
    else [SMAppService.mainAppService registerAndReturnError:&error];
    if (error) [self alert:@"Can't change \"Start at login\"" text:error.localizedDescription];
}

// --- the menu (made each time it opens) ---

- (NSMenuItem*)add:(NSMenu*)menu title:(NSString*)title action:(Action)block {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:block ? @selector(run:) : nil keyEquivalent:@""];
    item.target = self;
    item.representedObject = block ? [block copy] : nil;
    item.enabled = block != nil;
    [menu addItem:item];
    return item;
}

- (NSMenuItem*)add:(NSMenu*)menu title:(NSString*)title checked:(bool)on action:(Action)block {
    NSMenuItem* item = [self add:menu title:title action:block];
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return item;
}

- (void)run:(NSMenuItem*)sender {
    void (^block)(void) = sender.representedObject;
    if (block) block();
}

- (NSMenu*)submenu:(NSMenu*)menu title:(NSString*)title enabled:(bool)enabled {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    NSMenu* sub = [[NSMenu alloc] initWithTitle:title];
    sub.autoenablesItems = NO;
    item.submenu = sub;
    item.enabled = enabled;
    [menu addItem:item];
    return sub;
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    [self refreshState];
    [menu removeAllItems];
    const bool busy = _busy, mounted = _mounted;
    app::Settings& s = _settings;
    __weak MLVMenuBar* me = self;

    // The state, then what can be done about it.
    [self add:menu title:[self statusText] action:nil];
    [menu addItem:NSMenuItem.separatorItem];
    if (mounted) {
        NSString* at = toNS(_mountedAt);
        [self add:menu title:[NSString stringWithFormat:@"Open %@ in Finder", displayPath(_mountedAt)] action:^{
            [NSWorkspace.sharedWorkspace openURL:[NSURL fileURLWithPath:at]];
        }];
        if (!s.watch)
            [self add:menu title:@"Rescan the folders" action:when(!busy, ^{ notify_post("org.mlvdrive.rescan"); })];
        // FSKit can't tell the kernel what changed: names it has seen keep their first answer.
        [self add:menu title:@"Refresh (mount again)" action:when(!busy, ^{ [me remount]; })];
        [self add:menu title:[NSString stringWithFormat:@"Unmount %@", displayPath(_mountedAt)]
              action:when(!busy, ^{ [me unmountThen:nil]; })];
    } else {
        [self add:menu title:[NSString stringWithFormat:@"Mount on %@", displayPath([self mountPoint])]
              action:when([self canMount], ^{ [me mount]; })];
    }
    [menu addItem:NSMenuItem.separatorItem];

    // Folders, mount point, DNG options.
    if (s.folders.empty()) {
        [self add:menu title:@"Choose an MLV folder…" action:when(!busy, ^{ [me addFolder]; })];
    } else {
        NSString* label = s.folders.size() == 1
                              ? [@"MLV folder: " stringByAppendingString:shorten(displayPath(util::narrow(s.folders[0])), 48)]
                              : [NSString stringWithFormat:@"MLV folders: %zu", s.folders.size()];
        NSMenu* folders = [self submenu:menu title:label enabled:!busy];
        for (size_t i = 0; i < s.folders.size() && i < 50; ++i) {
            const std::wstring f = s.folders[i];
            [self add:folders title:[@"Remove " stringByAppendingString:shorten(displayPath(util::narrow(f)), 60)] action:^{
                MLVMenuBar* strong = me;
                if (!strong) return;
                auto& list = strong->_settings.folders;
                list.erase(std::remove(list.begin(), list.end(), f), list.end());
                [strong changed];
            }];
        }
        [folders addItem:NSMenuItem.separatorItem];
        [self add:folders title:@"Add a folder…" action:^{ [me addFolder]; }];
    }
    NSMenu* place = [self submenu:menu
                            title:[@"Mount point: " stringByAppendingString:displayPath([self mountPoint])]
                          enabled:!busy];
    [self add:place title:@"~/MLV Drive (the default)" checked:s.drive == L"~/MLV Drive" action:^{
        MLVMenuBar* strong = me;
        if (!strong) return;
        strong->_settings.drive = L"~/MLV Drive";
        [strong changed];
    }];
    [self add:place title:@"Choose a folder…" action:^{ [me chooseMountPoint]; }];

    NSMenu* dng = [self submenu:menu title:@"DNG" enabled:!busy];
    auto set = [me](void (^change)(app::Settings&)) -> Action {
        return ^{
            MLVMenuBar* strong = me;
            if (!strong) return;
            change(strong->_settings);
            [strong changed];
        };
    };
    [self add:dng title:@"16-bit uncompressed" checked:s.format == dng::Output::Uncompressed16
           action:set(^(app::Settings& x) { x.format = dng::Output::Uncompressed16; })];
    [self add:dng title:@"Uncompressed, packed to the clip's bit depth" checked:s.format == dng::Output::Uncompressed
           action:set(^(app::Settings& x) { x.format = dng::Output::Uncompressed; })];
    [dng addItem:NSMenuItem.separatorItem];
    [self add:dng title:@"Raw corrections (stripes, focus and bad pixels)" checked:s.rawFixes
           action:set(^(app::Settings& x) { x.rawFixes = !x.rawFixes; })];
    NSMenu* focus = [self submenu:dng title:@"Focus pixels" enabled:s.rawFixes];
    [self add:focus title:@"Auto (cameras with focus pixels, clips with their own .fpm)" checked:s.focusPixels < 0
           action:set(^(app::Settings& x) { x.focusPixels = -1; })];
    [self add:focus title:@"On" checked:s.focusPixels > 0 action:set(^(app::Settings& x) { x.focusPixels = 1; })];
    [self add:focus title:@"Off" checked:s.focusPixels == 0 action:set(^(app::Settings& x) { x.focusPixels = 0; })];
    NSMenu* bad = [self submenu:dng title:@"Bad pixels" enabled:s.rawFixes];
    [self add:bad title:@"Off" checked:s.badPixels == 0 action:set(^(app::Settings& x) { x.badPixels = 0; })];
    [self add:bad title:@"Auto (searched on the first frame)" checked:s.badPixels == 1
           action:set(^(app::Settings& x) { x.badPixels = 1; })];
    [self add:bad title:@"Aggressive (searched harder)" checked:s.badPixels == 2
           action:set(^(app::Settings& x) { x.badPixels = 2; })];
    [self add:bad title:@"From a map (.bpm)" checked:s.badPixels == 3 action:set(^(app::Settings& x) { x.badPixels = 3; })];
    [dng addItem:NSMenuItem.separatorItem];
    [self add:dng title:@"Timecode: time of day" checked:s.timecode == dng::Timecode::TimeOfDay
           action:set(^(app::Settings& x) { x.timecode = dng::Timecode::TimeOfDay; })];
    [self add:dng title:@"Timecode: from 00:00:00:00" checked:s.timecode == dng::Timecode::FrameCount
           action:set(^(app::Settings& x) { x.timecode = dng::Timecode::FrameCount; })];
    [self add:menu title:@"Test patterns (12 charts, 5 s each)" checked:s.testPatterns
           action:when(!busy, set(^(app::Settings& x) { x.testPatterns = !x.testPatterns; }))];
    [self add:menu title:@"Watch the folders for new clips" checked:s.watch
           action:when(!busy, set(^(app::Settings& x) { x.watch = !x.watch; }))];
    [menu addItem:NSMenuItem.separatorItem];

    // Setup: what macOS needs, once.
    const Tri enabled = moduleEnabled();
    const bool fda = fullDiskAccess();
    NSMenu* setup = [self submenu:menu
                            title:(enabled == Tri::No || !fda) ? @"Setup: something to turn on" : @"Setup"
                          enabled:true];
    [self add:setup
          title:enabled == Tri::Yes  ? @"File system extension: on"
                : enabled == Tri::No ? @"File system extension: off — turn it on…"
                                     : @"File system extension…"
         checked:enabled == Tri::Yes
          action:^{ [me openExtensionsSettings]; }];
    [self add:setup
          title:fda ? @"Full Disk Access: on" : @"Full Disk Access: off — needed for the Desktop, Documents, external drives…"
        checked:fda
         action:^{ [me openFullDiskAccess]; }];
    [self add:menu title:@"Mount when MLV Drive starts" checked:s.mountAtStart
           action:set(^(app::Settings& x) { x.mountAtStart = !x.mountAtStart; })];
    [self add:menu title:@"Start at login" checked:[self startsAtLogin] action:^{ [me toggleStartAtLogin]; }];
    [self add:menu title:@"Edit the settings file…" action:^{ [me editSettings]; }];
    [self add:menu title:@"Reload the settings file" action:when(!busy, ^{ [me reloadSettings]; })];
    [menu addItem:NSMenuItem.separatorItem];
    [self add:menu title:[NSString stringWithFormat:@"%@ %s", kAppName, MLVDRIVE_VERSION] action:nil];
    [self add:menu
          title:mounted ? [NSString stringWithFormat:@"Quit (unmounts %@)", displayPath(_mountedAt)] : @"Quit"
         action:^{ [NSApp terminate:nil]; }];
}

@end

int main() {
    @autoreleasepool {
        NSApplication* app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyAccessory;  // menu bar only (LSUIElement too)
        MLVMenuBar* delegate = [[MLVMenuBar alloc] init];
        app.delegate = delegate;
        [app run];
    }
    return 0;
}
