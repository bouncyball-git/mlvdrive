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
mlvdrive mount <MLV folder> [M:] [--dng=16|packed] [--no-fix] [--cache=MB] [--read-ahead=N] [--debug]
mlvdrive unmount M:
```

Every MLV clip under the folder (subfolders included) shows as a folder of DNG files,
`NAME\NAME_000000.dng` onward, made when they are read. Without a drive letter the next free one
from Z: down is used. Ctrl+C (or `mlvdrive unmount`) unmounts. WinFsp must be installed where it
runs.

Frames read in order are made ahead on every CPU (`--read-ahead`, default one per CPU, 0 off) and
kept in a cache (`--cache`, default 512 MB).

`mlvdrive_cli` runs the core without the drive:

```
mlvdrive_cli info  <clip.MLV> [frame]             clip details, one frame decoded (timings)
mlvdrive_cli dng   <clip.MLV> <frame> <out.dng>   one frame as a DNG
mlvdrive_cli check <clip.MLV> <frame> <file.dng>  a DNG read back and compared with the frame
mlvdrive_cli bench <clip.MLV> [frames]            ms a frame per stage, frames/s on every thread
    --dng=16|packed|lj92|pass   DNG format (default 16-bit)    --no-fix   no raw corrections
mlvdrive_cli selftest <clip.MLV>                  every frame: fast decoder and vector corrections
                                                  against the original code, value for value
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
