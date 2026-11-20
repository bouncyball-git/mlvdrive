# MLV Drive

Shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly as they are
read, so DaVinci Resolve (or any raw-capable app) imports MLV directly: no conversion, no extra
disk space. Windows first (WinFsp); Linux and macOS later. Work in progress: see `PLAN.md`.

## Building

Visual Studio 2022 (or its Build Tools) with the C++ tools, CMake and Ninja:

```
.\build.ps1            # build\mlvdrive_cli.exe
.\build.ps1 clean build
```

`mlvdrive_cli` runs the core without the drive:

```
mlvdrive_cli info  <clip.MLV> [frame]             clip details, one frame decoded (timings)
mlvdrive_cli dng   <clip.MLV> <frame> <out.dng>   one frame as a DNG
mlvdrive_cli check <clip.MLV> <frame> <file.dng>  a DNG read back and compared with the frame
mlvdrive_cli bench <clip.MLV> [frames]            ms a frame per stage, frames/s on every thread
    --dng=16|packed|lj92|pass   DNG format (default 16-bit)    --no-fix   no raw corrections
```

## Credits

- Inspired by and partly derived from MLVFS by David Milligan: the DNG tag set
  (`src/dng/dng_writer.cpp`) follows MLV-App's DNG writer, which grew out of MLVFS's.
- MLV decoding, the camera table and the raw pixel fixes come from MLV-App
  (`third_party/mlvapp`, see its README).

## Licence

GPL-3.0: see `LICENSE`.
