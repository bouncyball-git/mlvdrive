# MLV Drive

Shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly as they are
read, so DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra
disk space. Windows (WinFsp) and macOS 26 or later (FSKit, see [macOS](#macos)); Linux later.
Work in progress: see `PLAN.md`.

## Building

Visual Studio 2022 (or its Build Tools) with the C++ tools, CMake and Ninja, and
[WinFsp](https://winfsp.dev/rel/) with its Developer feature:

```
.\build.ps1             # build\mlvdrive_tray.exe, build\mlvdrive.exe, build\mlvdrive_tool.exe
.\build.ps1 clean build
.\build.ps1 install     # for the current user (no administrator rights): the programs into
                        # %LOCALAPPDATA%\Programs\MLV Drive, a Start Menu shortcut "MLV Drive"
.\build.ps1 uninstall   # removes them (the settings stay)
.\build.ps1 package     # dist\MLVDrive-<version>-win64.zip: portable (no installer)
.\build.ps1 setup       # dist\MLVDrive-<version>-setup.exe: the installer (needs Inno Setup 6)
.\build.ps1 -pkg        # build, then both: what a release needs (also --pkg; clean -pkg from scratch)
```

The version is set once, in the `VERSION` file; CMake, `build.ps1`, `build.sh` and the installers
all take it from there. For a release: change `VERSION`, commit, then tag that commit
`v<version>` (packaging stops when the commit carries a tag of another version). `tools/make_icons.py`
draws the icons in `assets/` (only needed to change them). The installer is
`installer/mlvdrive.iss`.

## Installing a release

Run `MLVDrive-<version>-setup.exe`:

- **Default** installs the tray app and the command-line drive; **Custom** chooses among the tray
  app, the command-line drive (`mlvdrive.exe`) and `mlvdrive_tool.exe`, with at least one of the
  first two (they are what shows the drive).
- For the current user (no administrator rights) or for all users.
- Options: desktop shortcut, start with Windows, add the command-line programs to PATH.
- MLV Drive needs [WinFsp](https://winfsp.dev/rel/). If it is missing (or older than 2.1), setup
  offers to download the latest version from winfsp.dev and install it; the downloaded installer
  is run only if it carries a valid signature of WinFsp's publisher (NAVIMATICS LLC). Without it,
  MLV Drive installs anyway and the tray app points to WinFsp.
- Uninstalling (Settings > Apps) asks whether to delete the settings too (and pixel maps of your
  own kept with them); WinFsp stays.

Silent installs: `/VERYSILENT /CURRENTUSER /COMPONENTS=gui,cli,tool /TASKS=addpath` (Inno Setup's
usual switches).

`MLVDrive-<version>-win64.zip` is the portable alternative: the same programs, run from any folder.

## The tray app

"MLV Drive" (`mlvdrive_tray.exe`) sits in the notification area: its icon is in colour while the
drive is mounted, grey when not. Right-click (or click) it for the menu: mount and unmount, open the
drive in Explorer (double-click does too), add and remove MLV folders, the drive letter, the DNG format,
raw corrections (focus and bad pixels too), timecode and the test patterns, watch the folders,
mount when it starts, start with Windows, edit the settings file. A change while mounted mounts
again with it. WinFsp must be installed (https://winfsp.dev/rel/); the menu says so if it isn't.

Clips added to, removed from or changed in the folders show on the drive by themselves (2 seconds
after the folders were last touched, so a clip being copied shows once it is complete), without
unmounting: files already open keep working, and Explorer refreshes. Folders on a network drive or
share, whose changes Windows may not report, are also looked at every 30 seconds (local ones
aren't: that would keep sleeping disks awake). With watching off, the menu has "Rescan the
folders"; `mlvdrive rescan M:` looks at once either way (for scripts).

## Settings

`%APPDATA%\MLV Drive\mlvdrive.ini` (UTF-8), written by the tray app, read by both programs:

```
[mount]
folder = E:\mlv             ; an MLV folder (subfolders included); one line each,
folder = G:\mlv             ; all their clips merged on the drive
drive = M:                  ; empty: the next free letter from Z: down
test_patterns = false
mount_at_start = true       ; the tray app mounts when it starts
watch = true                ; show clips added or changed in the folders as it happens
[dng]
format = 16                 ; 16 or packed
raw_fixes = true            ; raw corrections at all
focus_pixels = auto         ; auto (cameras with them, clips with their own .fpm), on, off
bad_pixels = off            ; off, auto, aggressive, map (.bpm)
timecode = time             ; time (of day) or count (from 00:00:00:00)
[performance]
cache_mb = auto             ; finished frames kept: auto (a quarter of the RAM) or MB
read_ahead = auto           ; auto (one per CPU) or a number of frames
```

Pixel maps (`.fpm` focus, `.bpm` bad pixels, MLV-App's format and names:
`<camera id>_<raw width>x<raw height>`) are looked up beside the clip, first as `<clip name>.fpm`
/ `.bpm` (a map for that clip alone; a clip-named `.fpm` turns the focus fix on in auto), then
under the generic name; then in `%APPDATA%\MLV Drive\pixelmaps` (macOS: `~/Library/Application
Support/MLV Drive/pixelmaps`); then in `pixelmaps` beside the programs. None come with MLV Drive:
for the EOS M, 100D, 650D and 700D the focus pixels of the usual video modes are worked out
without a map, and for the other modes (crop, other sizes) or the EOS M2, copy the maps you need
from MLV-App's collection, https://github.com/ilia3101/MLV-App/tree/master/pixel_maps (named by
camera id and raw size, as above), into `%APPDATA%\MLV Drive\pixelmaps` (macOS:
`~/Library/Application Support/MLV Drive/pixelmaps`), or beside the clips. Bad pixels are
searched on the first frame (or read from a `.bpm` map).

The cache keeps the frames read most recently, except that a clip read straight through keeps
its start: a clip longer than the cache, played again, finds that part made.

## Mounting from the command line

```
mlvdrive mount [MLV folder ...] [M:] [--dng=16|packed] [--no-fix|--fix] [--timecode=time|count]
               [--focus-pixels=auto|on|off] [--bad-pixels=off|auto|aggressive|map]
               [--test-patterns|--no-test-patterns] [--no-watch] [--cache=MB|auto] [--read-ahead=N] [--debug]
mlvdrive --version
mlvdrive mount --test-patterns [M:]
mlvdrive rescan M:
mlvdrive unmount M:
```

The settings file gives the defaults: with folders set there, `mlvdrive mount` alone mounts them.
Folders and options given on the command line override it. `mlvdrive rescan M:` and `mlvdrive
unmount M:` also work on the tray app's drive.

`--test-patterns` adds a `TestPatterns` folder of 12 test charts, 5 seconds each (1080p25,
timecode from 10:00:00:00): SMPTE RP 219 HD bars, SMPTE bars, EBU 100% and 75% bars, PLUGE, grey
scale, ramps, crosshatch, framing and safe areas, checkerboard, multiburst, zone plate. Their DNGs
state linear Rec.709 colour, so a raw developer with Rec.709 gamma shows the charts' levels.

Every MLV clip under the folders (subfolders included; several folders merge, a clip found twice
shows once, names that clash get " (2)") shows as a folder of DNG files,
`NAME\NAME_000000.dng` onward, made when they are read, and `NAME\NAME.wav` when the clip has
audio (Broadcast WAVE, lined up with the first frame). The DNG timecode and the WAV's time
reference are the time of day the clip was recorded (`--timecode=count`: from 00:00:00:00), so
editors can sync the audio by timecode. Without a drive letter the next free one from Z: down is
used. Ctrl+C (or `mlvdrive unmount`) unmounts. WinFsp must be installed where it runs.

Frames read in order are made ahead on every CPU (`--read-ahead`, default one per CPU, 0 off) and
kept in a cache (`--cache`, default auto: a quarter of the RAM, 512 MB to 16 GB).

`mlvdrive_tool` runs the core without the drive:

```
mlvdrive_tool info  <clip.MLV> [frame]             clip details, one frame decoded (timings)
mlvdrive_tool dng   <clip.MLV> <frame> <out.dng>   one frame as a DNG
mlvdrive_tool check <clip.MLV> <frame> <file.dng>  a DNG read back and compared with the frame
mlvdrive_tool bench <clip.MLV> [frames]            ms a frame per stage, frames/s on every thread
    --dng=16|packed|lj92|pass   DNG format (default 16-bit)    --no-fix   no raw corrections
mlvdrive_tool wav   <clip.MLV> <out.wav>           the clip's audio, as the drive shows it
mlvdrive_tool patterns <folder>                    one DNG of each test pattern (01.dng ... 12.dng)
    --timecode=time|count (dng, wav)   timecode: time of day (default) or from 00:00:00:00
mlvdrive_tool selftest <clip.MLV>                  every frame: fast decoder and vector corrections
                                                   against the original code, value for value;
                                                   every 32nd frame: the fast lossless encoder
                                                   against the original, byte for byte;
                                                   the audio read in pieces against read whole
mlvdrive_tool read  <folder>                       reads a mounted clip's DNGs in order: frames/s
    --threads=N  readers (default 1)   --passes=N (default 2)   --buffered  through Windows' cache
```

## Testing

```
.\tools\test.ps1 -Folders G:\mlv, E:\mlv [-Drive N:] [-Keep]
```

Runs the checks that matter before a release on real clips, in about a minute: every clip opens and
decodes; per kind of clip (camera, size, bit depth, compression; and a spanned one) every frame
through the fast decoder equals the original and the vector raw fixes the scalar ones; one frame in
each DNG format read back; the test charts; a mount (on a spare letter, every option given, so the
settings file doesn't matter) where each clip's middle frame equals a fresh decode, a WAV and a
chart equal `mlvdrive_tool`'s; the cache keeping a long clip's start; read speed above real time; and a
watched scratch folder where clips copied in, replaced and deleted show as they should while an
open file keeps reading. Prints PASS / FAIL per check; exit code 0 when all pass.

## macOS

MLV Drive for macOS 26 (Tahoe) or later, on Apple Silicon and Intel Macs: a menu bar app, `MLV
Drive.app`, with a built-in FSKit file system extension where the drive runs. Nothing else to
install: no macFUSE, no kernel extension.

### Installing

1. **Open the disk image** `MLVDrive-<version>-macos.dmg`, drag **MLV Drive** to
   **Applications**, and open it **from Applications**. (Not from the disk image, and don't move
   the app after opening it: see Troubleshooting.)
2. **Allow it to open.** MLV Drive isn't signed by a registered Apple developer, so macOS refuses
   it the first time: *"MLV Drive.app" Not Opened — Apple could not verify "MLV Drive.app" is
   free of malware...*. Click **Done** (not Move to Trash). Then open **System Settings >
   Privacy & Security**, scroll down to **Security**, where it says *"MLV Drive.app" was blocked
   to protect your Mac*, and click **Open Anyway**; confirm with your password or Touch ID, and
   **Open Anyway** once more. From then on it opens normally.
3. **First start.** MLV Drive appears in the menu bar (a drive labelled MLV) and asks for a folder
   of MLV clips: **Choose a Folder...** (clips in its subfolders count too; more folders can be
   added later). macOS asks whether MLV Drive may send notifications: allow them, they report a
   mount that fails while you aren't looking (at login).
4. **Turn on the file system extension** (once). The first mount says *Turn on MLV Drive's file
   system extension*: **Open System Settings** goes to **General > Login Items & Extensions**;
   there, click **ⓘ** beside **File System Extensions** and switch **MLV Drive** on. If it isn't
   listed, quit System Settings (⌘Q) and open it again: it lists what it found when it opened.
5. **Give it Full Disk Access** (once), for clips on the Desktop, in Documents or Downloads, in
   iCloud Drive or on external or network drives: macOS protects those places, and the extension
   can only read them with the app's Full Disk Access (allowing access to a single folder isn't
   passed on to it). MLV Drive's menu > **Setup** > *Full Disk Access: off* opens **Privacy &
   Security > Full Disk Access**: switch **MLV Drive** on, or add it with **+** (Applications >
   MLV Drive) if it isn't listed. Clips elsewhere (a folder of your own in your home folder, say)
   don't need it.
6. **Mount** from the menu. The drive shows in **`~/MLV Drive`** (your home folder; Finder > Go >
   Home), one folder per clip. MLV Drive's menu > **Setup** should now show both items on.
7. Optional, in the menu: **Start at login** (and **Mount when MLV Drive starts**, on by default,
   mounts it then), and **Setup > Install the mlvdrive command** (for Terminal; asks for an
   administrator's password).

### Using it

- **The icon:** white with a red light while mounted, grey while not. Click it for the menu: the
  state (clips, size), **Open ~/MLV Drive in Finder**, **Refresh (mount again)**, **Unmount**,
  the MLV folders (add, remove), the mount point, the DNG options (16-bit or packed, raw
  corrections, focus and bad pixels, timecode), test patterns, watching the folders, Setup, the
  settings file, **Quit** (unmounts).
- **The drive:** each clip is a folder of `NAME_000000.dng ...` files and `NAME.wav` when it has
  audio, made as they are read. In DaVinci Resolve, browse to `~/MLV Drive` in the Media Storage
  and add a clip's folder: it comes in as one clip. Any raw-capable app (Preview too) opens the
  DNGs.
- **Read-only:** nothing on the drive can be changed. Preview and others show the files as
  **Locked**: that only means they can't be saved in place (duplicate or export to edit a frame).
- **Changes:** clips copied into a watched folder show on the drive about 2 seconds after the copy
  finishes. A setting changed in the menu while mounted mounts the drive again with it (if a
  program has a file open, MLV Drive asks first). The settings are kept in
  `~/Library/Application Support/MLV Drive/mlvdrive.ini` (the same keys as on Windows; `drive` is
  the mount point).
- **The mount point** is `~/MLV Drive` by default, or a folder you choose: macOS doesn't let apps
  mount in /Volumes.
- **Focus pixels** (EOS M, M2, 100D, 650D, 700D): the usual video modes are fixed without a map;
  for the others, get the maps from https://github.com/ilia3101/MLV-App/tree/master/pixel_maps
  (`<camera id>_<raw width>x<raw height>.fpm`) and put them in
  `~/Library/Application Support/MLV Drive/pixelmaps` (or beside the clips). See Settings above.

### From the command line

After **Setup > Install the mlvdrive command** (or as
`/Applications/MLV\ Drive.app/Contents/MacOS/mlvdrive`):

```
mlvdrive mount [MLV folder] [--mount-point=PATH] [--dng=16|packed] [--no-fix] [--timecode=count]
               [--test-patterns] [--no-watch] [--cache=MB] [--read-ahead=N]
mlvdrive unmount [mount point]
mlvdrive rescan
mount -F -t mlvdrive <MLV folder> <mount point>     # the same, by hand (-o format=packed,...)
```

Without a folder, `mlvdrive mount` shows the settings' folders. The drive stays mounted until
unmounted (`mlvdrive unmount`, `umount`, Finder's eject or the menu).

### Limits

- **Replaced clips:** macOS can't be told what changed on an FSKit volume, and keeps the names it
  has looked up. Listings are always current and new clips always show, but a name already looked
  up keeps its first answer until the drive is mounted again: a clip replaced under the same name
  shows its old footage where it was open, a deleted one stays reachable by name. **Refresh
  (mount again)** in the menu fixes that. Adding clips, the usual case, isn't affected.
- **macOS 26 or later** (FSKit's folder-backed volumes are new in 26).

### Troubleshooting

- ***"MLV Drive.app" Not Opened*** at the first start: step 2 above.
- ***Turn on MLV Drive's file system extension*** / *the file system extension is turned off*:
  step 4 above.
- ***macOS couldn't start MLV Drive's file system extension*** (older versions: *Probing
  resource... com.apple.extensionKit.errorDomain error 2*): macOS still points to where MLV Drive
  was before it was moved, replaced by another copy or deleted. **Restart the Mac** (logging out
  isn't always enough). To avoid it: open MLV Drive only from Applications, and keep one copy.
- ***no MLV clips in the folders (or MLV Drive can't read them)***: check the folders in the menu
  have .MLV files; if they are on the Desktop, in Documents, in iCloud Drive or on an external or
  network drive, give MLV Drive Full Disk Access (step 5).
- ***macOS still holds the folders from an earlier mount***: try again in a moment; if it stays,
  restart the Mac.
- ***MLV Drive is in use*** when unmounting: a program (Resolve, Finder's preview) has a file open.
  Close it, or **Unmount Anyway** (the program loses the file).
- **A clip shows old footage:** see Limits; **Refresh (mount again)**.
- **The drive doesn't show in Finder's sidebar:** it is a folder in your home folder,
  `~/MLV Drive` (drag it to the sidebar's Favorites to keep it there).
- **More detail:** the extension's log, in Terminal:
  `log show --last 10m --predicate 'subsystem == "org.mlvdrive.fs"'`.

### Uninstalling

In this order (the extension's switch and Full Disk Access stay set after the app is gone):

1. **Quit** MLV Drive from its menu (it unmounts).
2. System Settings > General > Login Items & Extensions: **File System Extensions (ⓘ) > MLV Drive
   off**, and remove MLV Drive from **Open at Login** if it is there.
3. Privacy & Security > **Full Disk Access**: select MLV Drive, **−**.
4. Delete **MLV Drive** from Applications.
5. If wanted: the settings, `~/Library/Application Support/MLV Drive` (with any pixel maps of your
   own); the empty `~/MLV Drive` folder; the command, `sudo rm /usr/local/bin/mlvdrive`.
6. Before installing another copy somewhere else, restart the Mac (see Troubleshooting).

### Building

The Xcode Command Line Tools (`xcode-select --install`: Apple clang and the macOS 26 SDK; Xcode
itself isn't needed), CMake and Ninja (Homebrew):

```
./build.sh              # build/MLV Drive.app, build/mlvdrive_tool
./build.sh clean build
./build.sh install      # the release app (universal) into /Applications, in place of the one there
./build.sh uninstall    # removes it (turn its file system extension off first); keeps the settings
./build.sh package      # dist/MLVDrive-<version>-macos.dmg (tools/package-macos.sh)
./build.sh pkg          # build, then the disk image: what a release needs
```

- **One copy at a time:** two registered copies of the app compete for the extension. Without MLV
  Drive in /Applications, `build` registers `build/MLV Drive.app` (turn its extension on and give
  it Full Disk Access once, as in Installing); with it installed, `build` doesn't, and
  `./build.sh install` puts each build there. Removing or moving a copy that macOS has started:
  restart the Mac (see Troubleshooting).
- Each build updates the app in place and restarts its extension when no MLV Drive volume is
  mounted (otherwise the running one keeps its code until all are unmounted).
- Signing is ad hoc (`SIGN_IDENTITY` for a real identity), with designated requirements on the
  bundle identifiers, so macOS's permissions survive rebuilds.
- `install` and `package` build universal (arm64 + x86_64) in `build-release`; the disk image is
  LZMA-compressed (under 1 MB).
- By hand: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build`
  (`-DMLVDRIVE_RELEASE_BUNDLE=ON`: not registered).

### Testing

```
tools/test-macos.sh <MLV folder> [--all]
```

Mounts the folder with the test patterns and checks: each clip's frames read in random order
byte-identical to `mlvdrive_tool dng` (a sample, or every frame), each WAV identical to
`mlvdrive_tool wav`, a chart, a watched scratch folder where clips are added, replaced and
deleted, a rescan, the read speed, the unmount. PASS / FAIL per check; exit code: the failures.
It needs the development app (Building) and, for clips in protected places, its Full Disk Access.

## Credits

- Inspired by and partly derived from MLVFS by David Milligan: the DNG tag set
  (`src/dng/dng_writer.cpp`) follows MLV-App's DNG writer, which grew out of MLVFS's.
- MLV decoding, the camera table and the raw pixel fixes come from MLV-App
  (`third_party/mlvapp`, see its README).

## Licence

GPL-3.0: see `LICENSE`.
