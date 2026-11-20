# MLV Drive: plan

MLV Drive shows Magic Lantern MLV clips as a drive of CinemaDNG sequences, created on the fly
while they are read, so DaVinci Resolve (or any raw-capable app) imports MLV straight into the
Media Pool with its own raw controls: no conversion, no extra disk space.

```
E:\mlv\M21-1642.MLV  ──mount──►  M:\M21-1642\M21-1642_000000.dng
                                 M:\M21-1642\M21-1642_000001.dng
                                 ...
                                 M:\M21-1642\M21-1642.wav
```

It is a new program, in the spirit of MLVFS (David Milligan, 2014-2016) but not an upgrade of
it: MLVFS's filesystem layers (FUSE 2.6, Pismo File Mount, Dokan 1.0), its frame lookup, decoder,
caching and web GUI are all replaced. **Windows first** (WinFsp); Linux (FUSE 3) and macOS
(macFUSE / FSKit) later.

## Ground rules

- **Independent project.** It builds from this folder only. Code taken from elsewhere is
  **copied in** (once, at the start), never referenced by path at build time. After copying,
  the code, comments, README and build don't mention the projects it was copied from, except
  the required credits below.
- **Credits** (GPL): MLV-App (`third_party/mlvapp`, as its README there says) and MLVFS for
  anything ported from it (dual ISO, DNG tag choices): keep their copyright notices in ported
  files and name MLVFS in the README ("inspired by and partly ported from MLVFS by David
  Milligan").
- Licence: GPL-3.0 (compatible with MLV-App GPL-3.0 and MLVFS GPL-2.0-or-later).
- C++17, CMake + Ninja, MSVC (Visual Studio 2022 Build Tools are installed at
  `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`), static runtime
  (`/MT`), LF line endings.
- After every change, suggest a commit message; don't commit unless asked (see `CLAUDE.md`).

## What to copy in (step 1)

From `D:\devel\MLV-OFX` (tested; decodes the test clip correctly):

| From | To | What it is |
|---|---|---|
| `src/mlv/clip.{h,cpp}`, `src/mlv/mlv_format.h` | `src/mlv/` | MLV reading: blocks, frame index, spanned files `.M00…`, LJ92 decoding, metadata, audio (16-bit PCM aligned to the first video frame) |
| `src/mlv/color.{h,cpp}` | `src/mlv/` | camera matrices (ColorMatrix2), as-shot white balance, Kelvin/tint |
| `src/raw/rawfix.{h,cpp}` | `src/raw/` | raw corrections: vertical stripes, focus pixels, bad pixels, chroma smoothing (multi-threaded) |
| `src/util/file.{h,cpp}` | `src/util/` | UTF-8 paths, positioned reads from several threads |
| `src/parallel.{h,cpp}` | `src/` | thread pool (`par::forRange`) |
| `third_party/mlvapp/` (whole) | `third_party/mlvapp/` | MLV-App C code: fast LJ92 decoder **and encoder**, camera table, pixel fixes; its README and LICENSE |
| `build.ps1`, `CMakeLists.txt` | adapt | build script pattern (vcvars via vswhere, Ninja, stale-cache reset, elevated install) |

Engine speeds measured there (5D Mark III 1080p 14-bit LJ92, 8 threads): decode + raw corrections
+ a bilinear debayer = 22 ms a frame, so decode + corrections alone are below that.

From `D:\devel\MLVFS\mlvfs` (read, port what is needed; don't copy the whole tree):

| File | Use |
|---|---|
| `dng.c`, `dng_tag_*.h` | the DNG tag set that works in Resolve (Make/Model, CFA pattern, black/white levels, ColorMatrix1/2, AsShotNeutral, BaselineExposure, frame rate, timecode, ActiveArea/crop). Rewrite as `src/dng/` in C++ |
| `main.c` `get_mlv_basename`, `mlvfs_readdir` | Resolve naming: `<name>_%06d.dng` per frame, `<name>.wav` beside them; the optional BMD-style name `<name>_1_YYYY-MM-DD_0001_C0000` |
| `wav.c` | WAV header (with BWF timecode if it has one) |
| `hdr.c` (+ `amaze_demosaic_RT.c`, `cs.c`, `histogram.c`) | dual ISO (milestone 6) |
| `main.c` `deflicker`, `patternnoise.c` | optional extras, later |

Not taken: Pismo/Dokan/FUSE code, mongoose web server, jQuery, LZMA, GIF previews, `index.c`.

## Architecture

```
mlvdrive.exe  (console app; later a tray app)
 ├─ fs/winfsp_fs     WinFsp native API (FSP_FILE_SYSTEM_INTERFACE), read-only volume
 ├─ vfs/tree         the virtual tree: MLV folder → clip folders → DNG/WAV entries,
 │                   sizes and timestamps known without generating anything
 ├─ dng/writer       DNG header (fixed size per clip) + image data
 ├─ cache/frames     generated frames: LRU by bytes (e.g. 1 GB), read-ahead on the pool
 ├─ mlv/, raw/, util/, parallel   (copied, see above)
 └─ tools/cli        mlvdrive_cli: write one DNG to disk, bench a clip, dump the tree
```

- **Mount:** `mlvdrive mount <MLV folder> [M:]`. Scan the folder (and subfolders) once, open
  each clip once (frame index + metadata in memory), build the tree. Watch the folder for new
  or removed MLVs (later).
- **Read path:** a read of `M21-1642_000137.dng` at (offset, length) → get frame 137 from the
  cache or generate it (decode → corrections → DNG bytes) → copy the requested range. Many reads
  per file: keep the generated frame until the file handle closes (MLVFS does this too) and in
  the LRU after.
- **Read-ahead:** Resolve reads frames in order while playing. When frame N is requested, queue
  N+1…N+k (k ≈ threads) on the pool; cancel the queue on a jump. Bounded by the cache size.
- **Native WinFsp API, not its FUSE layer**: fewer copies and full control of caching
  (`FileInfoTimeout`, `CaseSensitiveSearch`, read-only flags, sector size, volume label).
- **Read-only**: Resolve doesn't need to write. MLVFS's `.MLD` overlay folder (for writes) is
  left out unless a need shows up.

## DNG output

The size of every file must be known when Explorer or Resolve lists the folder, before the frame
is generated:

1. **Uncompressed 16-bit** (MLVFS's format): size = header + w × h × 2, fixed. Works in Resolve
   (MLVFS proves it). About 4.1 MB a 1080p frame, ~100 MB/s at 24 fps through the filesystem.
   **Start with this.**
2. **Uncompressed, bit-packed at the clip's depth** (14/12/10-bit): fixed size, 12-37% less data.
   Test whether Resolve reads packed CinemaDNG.
3. **Lossless JPEG (compression 7)**: ~half the data, but the size varies per frame. Options:
   report a per-clip upper bound and pad after the image (StripByteCounts gives the real length;
   check Resolve only reads that), or generate on first listing (too slow). Research item:
   measure whether Resolve's decode time for LJ92 DNG outweighs the I/O saved. Our fast LJ92
   encoder makes the encode cheap.

The format is an option per mount (`--dng=16|packed|lj92`), default whichever measures fastest
in Resolve.

## Milestones

0. **Setup** (done)
   - `git init`, `.gitignore` (`build/`), `.gitattributes` (LF), `LICENSE` (GPL-3.0),
     `README.md` (credits), `CLAUDE.md`, this plan.
   - **WinFsp** with the **Developer** feature is installed at `C:\Program Files (x86)\WinFsp`
     (`inc\winfsp\winfsp.h`, `lib\winfsp-x64.lib`).
1. **Copy the core** (done): the table above, as CMake libraries `mlvapp_c` and `mlv_core`
   (which includes the thread pool). Change markers in `third_party/mlvapp` read "MLV Drive".
   `mlvdrive_cli info <clip> [frame]` checks it: the test clip opens with the right metadata;
   one frame decodes in ~8-11 ms, corrections ~4-6 ms.
   - Note: focus/bad pixel maps (`.fpm`) are opened with `fopen` from the current folder
     until `pixelproc_open_map` is set (not needed for the 5D Mark III).
2. **DNG writer + CLI**:
   - `mlvdrive_cli dng <clip> <frame> out.dng` (uncompressed 16-bit, raw corrections on).
   - Check: open the DNG in Resolve (import the file directly) and in another raw app;
     colour and orientation right; `mlvdrive_cli info` and `bench` (ms a frame for decode,
     corrections, DNG).
   - Read MLVFS `dng.c` for the tag set first.
3. **WinFsp mount, read-only, no cache**:
   - `mlvdrive mount E:\mlv M:`: folders, DNG names, sizes, timestamps from the clip's
     date; reads generate the frame.
   - Check in Resolve: the folder imports as one clip with the right frame rate and
     duration, raw controls in Camera Raw, playback.
4. **Speed**: per-handle frame, LRU cache, read-ahead, WinFsp tuning.
   - Measure: frames/s reading a mounted sequence (a CLI reader, then Resolve playback).
   - Target: real-time 1080p24 with corrections, and as close as possible at UHD.
5. **Audio + timecode**: `<name>.wav` (BWF with the clip's timecode), DNG TimeCodes tag, so
   Resolve syncs audio automatically.
6. **Dual ISO** (port from MLVFS `hdr.c`), then packed / LJ92 DNG (see above).
7. **Usability**:
   - Install script (copy exe, Start Menu shortcut).
   - Settings: command line + `mlvdrive.ini` (raw corrections, chroma smoothing, bad
     pixels, DNG format, cache size).
   - A tray icon with mount/unmount.
8. Later: Linux (FUSE 3), macOS (macFUSE or FSKit).

## Testing

- Test clip: `E:\mlv\M21-1642.MLV`: Canon 5D Mark III, 1920×1080, 14-bit lossless (LJ92),
  261 frames at 23.976 fps, ISO 800, WB 5000 K, stereo audio. Machine: 8 logical CPUs.
- Correctness: the image data in a generated DNG must equal the decoded + corrected raw frame
  (a CLI check that reads the DNG back); the DNG must open in Resolve and another raw app.
- Speed: `mlvdrive_cli bench` (per-stage ms), and a mounted-read benchmark (MB/s and frames/s
  sequentially, cold and warm cache).

## Open questions

- Does Resolve read bit-packed or LJ92 CinemaDNG from this kind of drive faster than 16-bit?
- Does Resolve need any write access to the clip folder (sidecar files)? MLVFS allowed writes
  into a real `.MLD` folder; start read-only and watch for errors.
- Which name scheme does Resolve group best: `<name>_000000.dng` or the BMD-style name?
- Resolve free vs Studio: CinemaDNG works in both; check playback in the one installed.
