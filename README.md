# MLV Drive

Shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly as they are
read, so DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra
disk space. Windows first (WinFsp); Linux and macOS later. Work in progress: see `PLAN.md`.

## Building

Visual Studio 2022 (or its Build Tools) with the C++ tools, CMake and Ninja, and
[WinFsp](https://winfsp.dev/rel/) with its Developer feature:

```
.\build.ps1             # build\mlvdrive_tray.exe, build\mlvdrive.exe, build\mlvdrive_cli.exe
.\build.ps1 clean build
.\build.ps1 install     # for the current user (no administrator rights): the programs into
                        # %LOCALAPPDATA%\Programs\MLV Drive, a Start Menu shortcut "MLV Drive"
.\build.ps1 uninstall   # removes them (the settings stay)
.\build.ps1 package     # dist\MLVDrive-<version>-win64.zip, for a GitHub release
```

The version is set once, in `CMakeLists.txt` (`project(... VERSION ...)`). `tools/make_icons.py`
draws the icons in `assets/` (only needed to change them).

## Installing a release

Unpack `MLVDrive-<version>-win64.zip` and run `install.cmd`: it installs MLV Drive for the current
user (no administrator rights) and starts it; `uninstall.cmd` removes it. MLV Drive needs
[WinFsp](https://winfsp.dev/rel/), installed separately.

## The tray app

"MLV Drive" (`mlvdrive_tray.exe`) sits in the notification area: its icon is in colour while the
drive is mounted, grey when not. Right-click (or click) it for the menu: mount and unmount, open the
drive in Explorer (double-click does too), add and remove MLV folders, the drive letter, the DNG format,
raw corrections (focus and bad pixels too), timecode and the test patterns, mount when it starts,
start with Windows, edit the settings file. A change while mounted mounts again with it. WinFsp must be installed
(https://winfsp.dev/rel/); the menu says so if it isn't.

## Settings

`%APPDATA%\MLV Drive\mlvdrive.ini` (UTF-8), written by the tray app, read by both programs:

```
[mount]
folder = E:\mlv             ; an MLV folder (subfolders included); one line each,
folder = G:\mlv             ; all their clips merged on the drive
drive = M:                  ; empty: the next free letter from Z: down
test_patterns = false
mount_at_start = true       ; the tray app mounts when it starts
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
               [--test-patterns] [--cache=MB|auto] [--read-ahead=N] [--debug]
mlvdrive --version
mlvdrive mount --test-patterns [M:]
mlvdrive unmount M:
```

The settings file gives the defaults: with folders set there, `mlvdrive mount` alone mounts them.
Folders and options given on the command line override it. `mlvdrive unmount M:` also unmounts the
tray app's drive.

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

`mlvdrive_cli` runs the core without the drive:

```
mlvdrive_cli info  <clip.MLV> [frame]             clip details, one frame decoded (timings)
mlvdrive_cli dng   <clip.MLV> <frame> <out.dng>   one frame as a DNG
mlvdrive_cli check <clip.MLV> <frame> <file.dng>  a DNG read back and compared with the frame
mlvdrive_cli bench <clip.MLV> [frames]            ms a frame per stage, frames/s on every thread
    --dng=16|packed|lj92|pass   DNG format (default 16-bit)    --no-fix   no raw corrections
mlvdrive_cli wav   <clip.MLV> <out.wav>           the clip's audio, as the drive shows it
mlvdrive_cli patterns <folder>                    one DNG of each test pattern (01.dng ... 12.dng)
    --timecode=time|count (dng, wav)   timecode: time of day (default) or from 00:00:00:00
mlvdrive_cli selftest <clip.MLV>                  every frame: fast decoder and vector corrections
                                                  against the original code, value for value;
                                                  the audio read in pieces against read whole
mlvdrive_cli read  <folder>                       reads a mounted clip's DNGs in order: frames/s
    --threads=N  readers (default 1)   --passes=N (default 2)   --buffered  through Windows' cache
```

## Credits

- Inspired by and partly derived from MLVFS by David Milligan: the DNG tag set
  (`src/dng/dng_writer.cpp`) follows MLV-App's DNG writer, which grew out of MLVFS's.
- MLV decoding, the camera table and the raw pixel fixes come from MLV-App
  (`third_party/mlvapp`, see its README).

## Licence

GPL-3.0: see `LICENSE`.
