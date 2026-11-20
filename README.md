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

`mlvdrive_cli info <clip.MLV> [frame]` prints a clip's details and decodes one frame.

## Credits

- Inspired by and partly ported from MLVFS by David Milligan.
- MLV decoding, the camera table and the raw pixel fixes come from MLV-App
  (`third_party/mlvapp`, see its README).

## Licence

GPL-3.0: see `LICENSE`.
