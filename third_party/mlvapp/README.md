From MLV-App (https://github.com/ilia3101/MLV-App), commit abdb10fd02dd (2026-11-11), `src/mlv/`:

- `liblj92/` (MIT, Andrew Baldwin): lossless JPEG; every change and how it was checked:
  `liblj92/lj92_changes.md`. Changed: the generic decoder's two row buffers
  overlapped for images of several components (the second started a row of one component in);
  its bit reader loaded past the data unchecked on a damaged stream (now zeros past the end); a
  Huffman table checked to fit its segment, zeroed where a code leaves it unfilled, and a second
  one no longer leaks the first; the header segments' bounds checked; a stream without a table
  refused (it crashed the fast decoder); the original encoder's row buffers checked, and a failed
  shrink of the encoded data keeps it (it was lost);
  `__builtin_clz` for MSVC; `extern "C"`;
  a fast decoder (`parseFast`) for predictor 1 and single-component predictor 6 streams written
  whole: a 64-bit bit reader refilling 8 bytes at a time without a branch where none is 0xFF,
  a combined 12-bit code + difference table (extra bits that don't fit read straight from the
  buffer, the full table only for codes over 12 bits), the prediction read from the output; and a
  fast encoder (`fastEncode`) for images read whole: each row's differences
  computed once, branch-free size categories and value bits, a 64-bit bit writer. The same values
  and the same bytes as the original (`lj92_fast = 0` uses it, for comparing).
- `camid/` (GPL): camera colour matrices and focal plane data. Changed: `extern "C"`; the
  accessors but `camidGet` left out (not used).
- `llrawproc/pixelproc.c`, `wirth.h` (GPL): focus and bad pixel fixes. Changed: pixel maps opened
  through `pixelproc_open_map`; a map read line by line (a line that wasn't two numbers, such as a
  comment after the first line, made the reader loop until memory ran out); a map pixel outside
  the frame no longer "fixes" one on a neighbouring row (reading up to 3 values past the image);
  the look-up tables' allocations checked; a file handle leak; chroma smoothing left out
  (`chroma_smooth*.c`, `opt_med.h`: not used); `extern "C"`.
- `pixelmaps/` (from `pixel_maps/`): MLV-App's focus pixel maps, all 273 (650D, 700D, EOS M,
  100D, EOS M2 in their video and crop modes; the collection grew from Daniel Fort's
  ml-focus-pixels maps), for the modes the built-in patterns don't cover. Changed: line endings
  to LF.
- `raw.h`: a portable stand-in for MLV-App's (whose bit fields need GCC's packed attribute).

Changes are marked "MLV Drive" in the code. MLV-App's licence: `LICENSE` (GPL-3.0).
