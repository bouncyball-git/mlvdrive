# MLV Drive

Shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly as they are
read, so DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra
disk space. Windows first (WinFsp); Linux and macOS later. Work in progress: see `PLAN.md`.

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
```

The version is set once, in `CMakeLists.txt` (`project(... VERSION ...)`). `tools/make_icons.py`
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
- Uninstalling (Settings > Apps) asks whether to delete the settings too; WinFsp stays.

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
under the generic name; then in `%APPDATA%\MLV Drive\pixelmaps`; then in `pixelmaps` beside the
programs (MLV-App's maps for EOS M and 700D modes). Bad pixels are searched on the first frame.

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
kept in a cache (`--cache`, default 512 MB).

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
chart equal the CLI's; the cache keeping a long clip's start; read speed above real time; and a
watched scratch folder where clips copied in, replaced and deleted show as they should while an
open file keeps reading. Prints PASS / FAIL per check; exit code 0 when all pass.

## Credits

- Inspired by and partly derived from MLVFS by David Milligan: the DNG tag set
  (`src/dng/dng_writer.cpp`) follows MLV-App's DNG writer, which grew out of MLVFS's.
- MLV decoding, the camera table and the raw pixel fixes come from MLV-App
  (`third_party/mlvapp`, see its README).

## Licence

GPL-3.0: see `LICENSE`.
