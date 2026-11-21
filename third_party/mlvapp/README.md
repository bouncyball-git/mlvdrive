From MLV-App (https://github.com/ilia3101/MLV-App), commit abdb10fd02dd (2026-11-11), `src/mlv/`:

- `liblj92/` (MIT, Andrew Baldwin): lossless JPEG. Changed: `__builtin_clz` for MSVC; `extern "C"`;
  a fast decoder (`parseFast`) for predictor 1 and single-component predictor 6 streams written
  whole: a 64-bit bit reader refilling 8 bytes at a time without a branch where none is 0xFF,
  a combined 12-bit code + difference table (extra bits that don't fit read straight from the
  buffer, the full table only for codes over 12 bits), the prediction read from the output; and a
  fast encoder (`fastEncode`) for images read whole: each row's differences
  computed once, branch-free size categories and value bits, a 64-bit bit writer. The same values
  and the same bytes as the original (`lj92_fast = 0` uses it, for comparing).
- `camid/` (GPL): camera colour matrices and focal plane data. Changed: `extern "C"`.
- `llrawproc/pixelproc.c`, `chroma_smooth*.c`, `opt_med.h`, `wirth.h` (GPL): focus and bad
  pixel fixes, chroma smoothing. Changed: pixel maps opened through `pixelproc_open_map`; a file
  handle leak; the fast chroma smoothing (`chroma_smooth_fast5.c`) made portable: its 4-lane
  vectors as GCC vector extensions, SSE2 or NEON, so MSVC builds use it too (it used the
  reference code: 4-5x slower; the same output); `extern "C"`.
- `pixelmaps/` (from `pixel_maps/`): MLV-App's focus pixel maps, all 273 (650D, 700D, EOS M,
  100D, EOS M2 in their video and crop modes; the collection grew from Daniel Fort's
  ml-focus-pixels maps), for the modes the built-in patterns don't cover. Changed: line endings
  to LF.
- `raw.h`: a portable stand-in for MLV-App's (whose bit fields need GCC's packed attribute).

Changes are marked "MLV Drive" in the code. MLV-App's licence: `LICENSE` (GPL-3.0).
