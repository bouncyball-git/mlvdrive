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
- **Where code comes from, in this order:** mlvtoolbox (`D:\devel\mlvtoolbox`, the most
  finished C++ versions), then MLV-App (`D:\devel\MLV-App`), then MLV-OFX. **MLVFS only when none
  of them has the code needed**; otherwise it is a reference for behaviour (naming, what Resolve
  accepts).
- **Credits** (GPL): MLV-App (`third_party/mlvapp`, as its README there says) and MLVFS for
  anything derived from it (MLV-App's DNG writer started as MLVFS's): keep their copyright
  notices in derived files and name MLVFS in the README ("inspired by and partly derived from
  MLVFS by David Milligan").
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

From `D:\devel\mlvtoolbox` (its `mlv/`, `raw/`, `util/` are the same as the ones above, byte for
byte):

| From | To | What it is |
|---|---|---|
| `src/dng/dng_writer.{h,cpp}` | `src/dng/` | **copied (milestone 2)**: MLV-App's DNG tag set in C++ (matrices, as-shot neutral, timecode, frame rate, reel name, pixel aspect, EXIF), packed / LJ92 / pass-through output. Added here: 16-bit output, the whole DNG in memory, sizes known per clip |
| `src/mlv/wav.{h,cpp}` | `src/mlv/` | **copied (milestone 5)**: Broadcast WAVE with bext/iXML. Changed here: the header built alone (fixed size per clip), the audio read by range |

From `D:\devel\MLV-App`: `darkframe.c`, `patternnoise.c` as optional extras. (Dual ISO is out
of the project for good.)

From `D:\devel\MLVFS\mlvfs`: a reference only (see the order above). Behaviour to match: the
names Resolve groups, from `main.c`: `<name>_%06d.dng` per frame, `<name>.wav` beside them; the
optional BMD-style name `<name>_1_YYYY-MM-DD_0001_C0000`.

## Architecture

```
mlvdrive_tray.exe (tray app) / mlvdrive.exe (console), on app/settings + app/mounter
 ├─ fs/winfsp_fs     WinFsp native API (FSP_FILE_SYSTEM_INTERFACE), read-only volume
 ├─ vfs/tree         the virtual tree: MLV folder → clip folders → DNG/WAV entries,
 │                   sizes and timestamps known without generating anything
 ├─ dng/writer       DNG header (fixed size per clip) + image data
 ├─ cache/frames     generated frames: LRU by bytes (e.g. 1 GB), read-ahead on the pool
 ├─ mlv/, raw/, util/, parallel   (copied, see above)
 └─ tools/mlvdrive_tool  mlvdrive_tool: write one DNG to disk, bench a clip, dump the tree
```

- **Mount:** `mlvdrive mount <MLV folder> [M:]`. Scan the folder (and subfolders) once, open
  each clip once (frame index + metadata in memory), build the tree. The folders are watched:
  clips added, removed or changed show without remounting (done in 0.2.0, see milestone 7).
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
is generated. The header is the same size for every frame of a clip (1200 bytes for the test
clip: every value that changes per frame has a fixed size), so a file is header + image.

1. **Uncompressed 16-bit** (MLVFS's format): size = header + w × h × 2, fixed. Works in Resolve
   (MLVFS proves it). About 4.1 MB a 1080p frame, ~100 MB/s at 24 fps through the filesystem.
   **Start with this.** The frame is decoded and corrected straight into the file's buffer.
2. **Uncompressed, bit-packed at the clip's depth** (14/12/10-bit): fixed size, 12-37% less data.
   Written already (`--dng=packed`); test whether Resolve reads packed CinemaDNG.
3. **Lossless JPEG (compression 7)**: ~half the data, but the size varies per frame. Written
   already (`--dng=lj92`; `--dng=pass` is the recorded LJ92 as it is, without corrections). Options:
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
   `mlvdrive_tool info <clip> [frame]` checks it: the test clip opens with the right metadata;
   one frame decodes in ~8-11 ms, corrections ~4-6 ms.
   - Note: focus/bad pixel maps (`.fpm`) are opened with `fopen` from the current folder
     until `pixelproc_open_map` is set (not needed for the 5D Mark III).
2. **DNG writer + CLI** (done but for the Resolve check):
   - `src/dng/dng_writer` from mlvtoolbox, plus 16-bit output and rendering into memory.
   - `mlvdrive_tool info | dng | check | bench`, `--dng=16|packed|lj92|pass`, `--no-fix`.
   - Checked: `check` reads every format back equal to the decoded + corrected frame (and finds
     the difference against an uncorrected one); exiftool validates all four; LibRaw (rawpy)
     develops them identically, right way up, natural colour.
   - Test clip, one thread: decode 8.3 ms + corrections 3.9 ms, whole 16-bit DNG 12.3 ms;
     8 threads: 400 frames/s 16-bit (17x real time), 267 packed, 186 LJ92, 1420 pass-through.
   - Resolve check moved to later (see "Resolve, later" below); a viewer is enough for now.
3. **WinFsp mount, read-only, no cache** (done but for the Resolve check):
   - `mlvdrive mount <folder> [M:] [--dng=16|packed] [--no-fix] [--debug]`; no letter: the
     next free one from Z: down. Ctrl+C unmounts.
   - `src/vfs/tree`: every `.MLV` under the folder (opened on all threads), subfolders that
     hold clips kept, the rest left out; a clip folder named like an existing folder gets
     " (2)". Names compare case-insensitively; dates from the clip's RTCI (frames apart by
     their timestamps).
   - `src/fs/winfsp_fs`: native API, read-only volume, one security descriptor (read for
     everyone), `FileInfoTimeout` infinite (the tree never changes: Windows caches names,
     sizes and file data). WinFsp passes opens on only if Create and Overwrite exist: stubs
     that answer "write protected". The DLL is delay-loaded from WinFsp's install folder.
   - A read within the header makes only the header; anything further makes the whole DNG,
     kept by the open file until it closes.
   - The raw fixer now always measures stripes (and searches bad pixels) on frame 0, whatever
     frame comes first: before that, a frame's bytes depended on the order frames were read.
   - Checked: every frame read from the drive, in any order, is byte-identical to
     `mlvdrive_tool dng`; nested folders, the name clash, case-insensitive lookups, wrong frame
     names, writes refused. Reading the sequence in order: 44.5 frames/s (176 MB/s, 1.9x real
     time) made on the drive, 969 frames/s again from Windows' cache.
   - Acceptance for now: browse the virtual folders in Explorer and open the DNGs in a viewer
     (LibRaw develops frames read from the drive correctly). Resolve comes later.
   - **Found after:** an open file's frame lives until WinFsp's Close, which with Windows
     caching file data comes only when the cache drops the file: the process grew to ~1 GB
     (every frame read). Fix first in milestone 4: free it at Cleanup.
4. **Speed** (done):
   - `src/vfs/frame_cache`: finished DNGs shared by all readers, LRU limited in bytes
     (`--cache=MB`, default 512); a frame being made is waited for, never made twice.
   - Read-ahead on one worker per CPU (`--read-ahead=N`, default one per CPU, 0 off): a read
     just after the previous one (going forward) queues the next N frames; a jump drops the
     queue and starts nothing until reads go forward again (random reads made 8 frames each
     for nothing before that rule).
   - An open file holds a reference into the cache, released at Cleanup
     (`PostCleanupWhenModifiedOnly = 0`); reads after it (from Windows' cache) fetch again.
     Memory now stays at the cache size (552 MB with 512 MB of cache; it grew to ~1 GB before).
   - `mlvdrive unmount M:` (a named event the mount waits on) unmounts cleanly and prints the
     cache's counts; `mlvdrive_tool read <folder> [--threads=N] [--passes=N] [--buffered]`
     reads a mounted sequence unbuffered (what the drive itself gives).
   - Test clip, 16-bit with corrections, unbuffered, one reader in order: 49.5 frames/s without
     read-ahead -> 142 frames/s first pass, 261 second (11x real time); 2 readers 268 (the CPU
     is the limit); every frame made ahead was read. Random order: 23 ms a frame. Frames made
     ahead are byte-identical (`check`).
   - Making a frame faster (after milestone 4; output unchanged, `mlvdrive_tool selftest`
     compares every frame value for value with the original decoder and the scalar fixes):
     - Stripe correction in SSE4.1 (run-time check, scalar otherwise; `raw::setSimd`): its two
       passes (brightest pixel 1.5 ms, factors 2.4 ms) were scalar compute, not memory (a plain
       pass is 0.5 ms). 3.9 -> 1.2 ms. Merging it into the decode can't be exact: it needs the
       frame's brightest pixel (18,708 pixels sit at it in frame 0) before any row.
     - LJ92 bit reader: 8-byte refills without a branch where none is 0xFF, long values' extra
       bits read inline (3.2% of samples took a second table lookup). Decode 8.2 -> 6.3 ms.
       Copying the data without stuffing first was as fast on one thread but 5% slower on 8:
       its memory traffic.
     - Test clip (i7-4790K, 4 cores / 8 threads): a whole 16-bit DNG 12.3 -> 7.7 ms on one
       thread; 400 -> ~540 frames/s on 8. Through the drive one reader stays ~270 frames/s:
       moving 4 MB files through WinFsp is now the limit there.
   - Not done: WinFsp tuning; decoding one frame on several threads (Huffman
     resynchronisation: ~4-5x lower latency per frame, no throughput gain). Worth measuring on a
     UHD / 5.7K clip first.
5. **Audio + timecode** (done; Resolve's automatic sync to check later):
   - `NAME.wav` beside the DNGs when the clip has audio: Broadcast WAVE (bext, iXML) from
     mlvtoolbox's `wav.cpp`. Its size is known at mount (header fixed per clip + the audio for
     the frames' duration at the recorded rate); reads come from just the audio blocks they
     need (`Clip::readSyncedAudioRange`, `syncedAudioBytes`: readSyncedAudio's arithmetic
     without reading), silence where the recording runs out.
   - Timecode: time of day by default (`--timecode=time|count`, CLI and mount): DNG TimeCodes =
     RTCI start time + frame number, counted at round(fps) (24 for 23.976, non-drop); the WAV's
     bext TimeReference = frame 0's timecode frame x rate / fps (MLV-App's formula), so the two
     mark the same moment. `Writer::timecodeFrame`, `frameRate` give the numbers.
   - `mlvdrive_tool wav <clip> out.wav` writes the drive's WAV; `selftest` also compares the
     audio read in pieces with it read whole.
   - Checked on the test clip: audio 10.886 s = 261 frames at 23.976; frame 0 16:42:59:00,
     frame 260 16:43:09:20; TimeReference 2,891,483,483 = frame 1,444,296 = 16:42:59:00;
     ffprobe / exiftool read it; the drive's WAV is byte-identical to the CLI's, in any range.
6. **Test patterns** (done; dual ISO, planned here before, is dropped for good):
   - `mlvdrive mount [folder] [M:] --test-patterns`: a `TestPatterns` clip folder (the MLV folder
     may be left out): 12 charts, 5 s each, 1920 x 1080 at 25 fps (1500 frames, 60 s), timecode
     from 10:00:00:00 (`--timecode=count`: 00:00:00:00). SMPTE RP 219 HD bars, SMPTE bars (SD
     layout), EBU 100% and 75% bars, PLUGE, 11-step grey scale, grey / R / G / B ramps,
     16 x 9 crosshatch with circle, framing and safe areas (93% action, 90% title, 4:3, 1.85,
     2.39), checkerboard, multiburst, zone plate.
   - `src/vfs/source.h`: a clip folder's frames come from a `FrameSource` (an MLV clip through
     its writer, or the patterns); `src/vfs/test_patterns`: each chart drawn once in video
     levels, taken back through the BT.709 curve to linear light, mosaicked RGGB (16-bit, black
     4096, white 60000: room for sub-black), kept in memory; a frame = header + that image.
   - `dng::generatedHeader`: a DNG whose camera RGB is linear Rec.709 (ColorMatrix1 = XYZ ->
     sRGB, D65; ForwardMatrix1 = sRGB -> XYZ D50; AsShotNeutral 1 1 1), Make "MLV Drive",
     Model "Test Pattern".
   - `mlvdrive_tool patterns <folder>` writes one frame of each.
   - Checked: exiftool validates; LibRaw with Rec.709 gamma gives the charts' levels (EBU 75%
     bars 191/0, white 255; grey scale 0, 25, 51 ... 255); full-resolution looks right (the zone
     plate's colour moire past Nyquist is the Bayer sampling it is there to show); on the drive:
     1500 files, timecodes 10:00:00:00 to 10:00:59:24, each chart's image equal to the CLI's,
     128 frames/s reading in order, alongside MLV clips in one mount.
   - Packed / LJ92 DNG on the drive (see "DNG output") wait for the Resolve questions.
7. **Usability** (done):
   - Settings: `%APPDATA%\MLV Drive\mlvdrive.ini` (UTF-8; `src/app/settings`): folder, drive,
     test patterns, mount at start, DNG format, raw fixes, timecode, cache, read-ahead. The
     console program takes it as defaults (options override; `mlvdrive mount` alone works).
   - `src/app/mounter`: tree + cache + drive as one object, used by both programs (library
     `mlvdrive_app` with the WinFsp layer).
   - Tray app `mlvdrive_tray.exe` (Win32, no console; `src/tray`): colour / grey icon
     (mounted / not; `tools/make_icons.py` -> `assets/`), menu: mount, unmount, open in
     Explorer (or double-click), MLV folder (folder picker), drive letter (letters in use
     greyed), DNG format, raw corrections, timecode, test patterns, mount at start, start with
     Windows (HKCU Run), edit / reload the settings file, quit. Mounting on a worker thread; a
     change while mounted remounts; balloons for errors, skipped clips and first run; one
     instance; icon back after Explorer restarts; `mlvdrive unmount M:` reaches it (the same
     named event). Manifest: common controls 6, per-monitor DPI.
   - `build.ps1 install | uninstall`: per user, no administrator rights:
     `%LOCALAPPDATA%\Programs\MLV Drive`, Start Menu "MLV Drive", warns without WinFsp; stops a
     running copy first (its Quit, then force after 5 s); uninstall removes "Start with
     Windows" too and keeps the settings.
   - Checked: the tray app mounts at start from the settings, every menu action (sent as its
     command) changes the drive and the file as it should, quit unmounts; the console mounts
     from the file alone; install / uninstall / reinstall while running.
   - Several MLV folders (after milestone 7): `folder =` once per folder in the settings,
     `mlvdrive mount A B [M:]`, the tray menu adds (folder picker) and removes them. Their clips
     merge at the drive's root (subfolders kept; a clip found twice, as with a folder inside
     another, shows once, by its full path; clashing names get " (2)"); a folder that can't be
     read is reported and left out. Checked: E:\mlv + G:\mlv + G:\mlv\Tipebi_Asorti: 54 clip
     folders, no duplicates, 304 ms; the tray app keeps both folders when it saves.
   - Cache (after milestone 7): within a forward run through a clip (reads in order since the
     last jump) the newest frame already read is dropped instead of the least recently used, so
     a clip longer than the cache keeps its start for the next pass; other clips and older
     places still go least-recently-used first; unread read-ahead frames are kept. Default size
     `cache_mb = auto`: a quarter of the RAM, 512 MB to 16 GB. Checked: a 479-frame clip read
     3 times through 512 MB: 232 frames found made on passes 2-3 (about 116 a pass; plain LRU
     found none), every read-ahead frame read.
   - Focus and bad pixels as settings (after milestone 7; chroma smoothing left out on purpose):
     `focus_pixels = auto|on|off`, `bad_pixels = off|auto|aggressive|map`, in the ini, on the
     console and in the tray's DNG menu. Pixel maps looked up beside the clip (`<clip>.fpm` /
     `.bpm`, then the generic `<camera>_<w>x<h>` name), then `%APPDATA%\MLV Drive\pixelmaps`,
     then `pixelmaps` beside the programs (MLV-App's 7 maps, copied into
     `third_party/mlvapp/pixelmaps`, installed with the programs); a clip-named `.fpm` turns the
     focus fix on in auto. Checked on `M05-1137` (5D Mark III with its own .fpm): 24,697 pixels
     changed, all of them map points; aggressive bad-pixel search finds 10 in a frame (auto
     none); selftest identical vector / scalar; the drive gives the same bytes read out of order
     (the search runs on frame 0).
   - Release (after milestone 7): version 0.1.0 from CMakeLists.txt in every program
     (`mlvdrive --version`, help, tray tooltip and menu), the tray's resources and the DNG
     Software tag. `tools/install.ps1` (+ `install.cmd` / `uninstall.cmd`, CRLF: batch files)
     installs per user from its own folder; `build.ps1 install` uses it from `build\stage`;
     `build.ps1 package` makes `dist\MLVDrive-<version>-win64.zip` (2.8 MB). Checked: installed
     from the unpacked zip over a running copy, started, `M:` back with every frame checked.
     Version 0.2.0 since.
   - Installer (0.2.0): `installer/mlvdrive.iss`, `build.ps1 setup` ->
     `dist\MLVDrive-<version>-setup.exe` (2.5 MB; Inno Setup 6). Types Default (tray app +
     command-line drive) and Custom (gui / cli / tool components; at least one of gui and cli,
     checked on the components page and before installing, also when silent). Per user by
     default (`{autopf}`: %LOCALAPPDATA%\Programs), all users on request. Tasks: desktop
     shortcut, start with Windows (the tray's Run value), PATH (user or machine, removed on
     uninstall). Upgrades: programs using the files are closed (Restart Manager; the tray app
     unmounts on WM_ENDSESSION), deselected programs and old names removed. WinFsp: detected
     (HKLM32\SOFTWARE\WinFsp InstallDir, bin\winfsp-x64.dll version >= 2.1); if not, a page
     offers to download the latest from winfsp.dev: the page's first link to
     github.com/winfsp/winfsp/releases/download/.../winfsp-*.msi, run (msiexec, elevated) only
     if Get-AuthenticodeSignature says Valid and signed by NAVIMATICS LLC (its module loaded by
     path: started from PowerShell 7, setup passes a module path that breaks Windows PowerShell's
     autoload); any failure: MLV Drive installs anyway, winfsp.dev offered. Uninstall: quits the
     tray app only if it runs from this installation (WMI), removes "start with Windows" only if
     it points here, asks before deleting the settings, leaves WinFsp. The zip is portable now
     (no install scripts); `build.ps1 install` keeps using tools/install.ps1 for development.
     Checked (silent, into a scratch folder, beside the real install): tool alone refused;
     everything + PATH installs; /TESTWINFSP finds 2.1.25156 on winfsp.dev, downloads it,
     verifies NAVIMATICS LLC; gui alone removes the other programs; uninstall removes files,
     PATH entry and Add/Remove entry; the running tray app of the other installation untouched.
     Not tested: the wizard by hand, and WinFsp's actual install (it is installed here).
   - Live folders (0.2.0): a rescan builds a new tree from the old one: clips whose files have
     the same size and date (every chunk) are taken over as they are (same ClipEntry, frames
     still cached), new or changed ones opened. The drive swaps trees (`Drive::update`): opens
     from then on use the new one, open files keep theirs until closed; Windows is notified of
     every folder and file added, removed or changed (names upper case: no name normalisation),
     which also drops its cached listings and sizes. Clip ids are never reused (the frame
     cache's key; index numbers), a changed or removed clip's frames are forgotten. Watching
     (`watch = true`, `--no-watch`): a thread per folder on ReadDirectoryChangesW (subtree), a
     missing folder retried every 5 s; a rescan 2 s after the last change (a copy in progress
     waits). Folders on a network drive or UNC path, or that Windows won't watch, are also
     rescanned every 30 s (SMB notifications are unreliable); local ones not (sleeping disks).
     `mlvdrive rescan M:` (named event, also for the tray's drive) at once; the tray's "Rescan
     the folders" only shows with watching off. (Checked: a \\localhost\G$ path is seen as
     network and polled; its changes still came by notification in 2.1 s. The 30 s timer itself
     wasn't seen firing: no server without notifications to test against.) Checked: a clip copied in appears 2 s after, verified; a
     clip replaced by other footage under the same name updates (124 -> 542 frames, frame 300
     verified) while a handle opened before keeps reading; a deleted clip disappears; listings
     are fresh after each change.
8. Later: Linux (FUSE 3), macOS (macFUSE or FSKit).

**Resolve, later** (when the user wants to run it): import `M:\<clip>` as one clip with the
right frame rate and duration, CinemaDNG raw controls, playback in real time; then the open
questions below (packed / LJ92 DNG, naming, write access).

## Testing

- **`tools\test.ps1 -Folders G:\mlv, E:\mlv`** runs the checks below automatically (about a
  minute; PASS / FAIL, exit code): clips open; selftest per kind and a spanned clip; the four DNG
  formats read back; the charts; a mount's frames, WAV and chart against the CLI; the cache
  keeping a long clip's start and real-time reads; a watched folder's add / replace / delete
  with an open file. Its mounts take a spare letter and give every option (`--no-test-patterns`
  was added for that). First run (0.2.0): 20 of 20 passed in 60 s. Run it before every release.
- Test clip: `E:\mlv\M21-1642.MLV`: Canon 5D Mark III, 1920×1080, 14-bit lossless (LJ92),
  261 frames at 23.976 fps, ISO 800, WB 5000 K, stereo audio. Machine: 8 logical CPUs.
- Test set `G:\mlv` (SATA SSD; 52 clips, ~70 GB, in subfolders), all 14-bit 23.976 fps:
  5D Mark III 1080p LJ92 1:1 (RAWC, audio) x5; 5D Mark III 3072x1728 LJ92 1:1 x2;
  5D Mark III 1080p uncompressed without RAWC x20 (one spanned: `M05-1137.MLV` + `.M00`, 1379
  frames; some with audio); Canon 60D 1280x544 uncompressed without RAWC x25.
  Checked (after milestone 7): every clip opens and decodes (`info`); `selftest` (every frame:
  decoders, vector vs scalar fixes, audio by range) passes on one clip of each kind and the
  spanned one; geometry right without RAWC (60D square pixels, 2.35:1 frame; 1:1 crop focal
  plane values); LibRaw develops each kind correctly; the whole folder mounts in 236 ms (75 GB
  of DNG, subfolders and WAVs as expected) and every clip's middle frame read from the drive
  equals a fresh decode + fixes. Reading in order through the drive: 3K LJ92 72.6 frames/s,
  1080p LJ92 119, 1080p uncompressed 98 (cold, ~350 MB/s from the SSD) to 126, 60D 164: all
  3x to 7x real time. No bugs found. (`M05-1137.fpm` beside its clip is not used: bad-pixel
  fixing is off by default and maps are looked up by camera, not by clip name.)
- Correctness: the image data in a generated DNG must equal the decoded + corrected raw frame
  (a CLI check that reads the DNG back); the DNG must open in Resolve and another raw app.
- Speed: `mlvdrive_tool bench` (per-stage ms), and a mounted-read benchmark (MB/s and frames/s
  sequentially, cold and warm cache).

## Open questions

- Does Resolve read bit-packed or LJ92 CinemaDNG from this kind of drive faster than 16-bit?
- Does Resolve need any write access to the clip folder (sidecar files)? MLVFS allowed writes
  into a real `.MLD` folder; start read-only and watch for errors.
- Which name scheme does Resolve group best: `<name>_000000.dng` or the BMD-style name?
- Resolve free vs Studio: CinemaDNG works in both; check playback in the one installed.
