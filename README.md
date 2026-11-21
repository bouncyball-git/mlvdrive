# MLV Drive

Shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly as they are
read, so DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra
disk space. Windows first (WinFsp); Linux and macOS later. Work in progress: see `PLAN.md`.

## Building

Visual Studio 2022 (or its Build Tools) with the C++ tools, CMake and Ninja, and
[WinFsp](https://winfsp.dev/rel/) with its Developer feature:

```
.\build.ps1            # build\mlvdrive.exe, build\mlvdrive_cli.exe
.\build.ps1 clean build
```

## Mounting

```
mlvdrive mount <MLV folder> [M:] [--dng=16|packed] [--no-fix] [--timecode=time|count]
               [--test-patterns] [--cache=MB] [--read-ahead=N] [--debug]
mlvdrive mount --test-patterns [M:]
mlvdrive unmount M:
```

`--test-patterns` adds a `TestPatterns` folder of 12 test charts, 5 seconds each (1080p25,
timecode from 10:00:00:00): SMPTE RP 219 HD bars, SMPTE bars, EBU 100% and 75% bars, PLUGE, grey
scale, ramps, crosshatch, framing and safe areas, checkerboard, multiburst, zone plate. Their DNGs
state linear Rec.709 colour, so a raw developer with Rec.709 gamma shows the charts' levels.

Every MLV clip under the folder (subfolders included) shows as a folder of DNG files,
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
