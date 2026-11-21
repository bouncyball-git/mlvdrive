# liblj92: what MLV Drive changed

`lj92.c` and `lj92.h` (lossless JPEG, MIT, Andrew Baldwin) come from MLV-App
(https://github.com/ilia3101/MLV-App, commit abdb10fd02dd, `src/mlv/liblj92/`). Every change is
marked "MLV Drive" in the code. This file lists them all: what was changed and why.

## Portability

| Where | Change |
|---|---|
| `lj92.c`, top | `__builtin_clz` for MSVC (`lj92_clz`, through `_BitScanReverse`; 32 for 0). |
| `lj92.h` | `extern "C"` for the C++ callers; `<stdint.h>` included; `lj92_fast` declared. |

## Speed

Both fast paths give exactly what the original code gives: the same values decoding, the same
bytes encoding. The original code is kept and still runs where the fast path doesn't apply.

| Where | Change |
|---|---|
| `lj92_fast` | 1 (the default): the fast decoder and encoder where they apply. 0: the original code, for comparing. `mlvdrive_tool selftest` decodes every frame both ways and encodes every 32nd frame both ways, and fails on any difference. |
| `parseFast()` (with `lj92_bits`, `lj92_refill`, `lj92_refill_bytes`, `lj92_fill_fast`, `lj92_diff_fast`, `lj92_diff_slow`) | A decoder for predictor 1 with 1 to 4 components (camera MLV frames: 2 components), and for predictor 6 with 1 component (MLV-App's own frames). The image must be written whole: no linearisation table, no skipping. A 64-bit bit reader refills 8 bytes at once when none of them is 0xFF, byte by byte otherwise (stuffed bytes, markers, the end; past the end it reads zeros). A 12-bit table gives the code and its difference in one lookup; extra bits that don't fit are read from the buffer, and codes longer than 12 bits use the full table. The prediction is read from the output image. |
| `parseScan()` | Takes the fast decoder when it applies. Predictor 6 with several components is left to the original code: there, "left" comes from any component. |
| `fastEncode()` (with `lj92_row_diffs`, `lj92_ssss`) | An encoder for an image read whole (no delinearisation, no skipping). Each row's differences are computed once; the size categories and value bits need no branch; a 64-bit bit writer stores 4 bytes at once when none is 0xFF. It uses the same histogram, table, header and bits as `frequencyScan` + `writeBody`. |
| `lj92_encode()` | Takes the fast encoder when it applies. |

## Fixes

Almost all of these only show on damaged data: a frame cut short or overwritten on the card, or a
bad file. Valid frames decode as before. Before these fixes, such a frame could crash the drive
(the tray app or `mlvdrive`), not just fail to decode.

| Where | Problem | Now |
|---|---|---|
| `lj92_open()`, row buffers | `outrow[1]` started at `rowcache[x]`, but a row is `x * components` values, so with 2 components the two rows overlapped. Only the original decoder uses them; it decoded predictor 1 right anyway, but the other predictors read overwritten rows. | `rowcache[x * components]`. |
| `nextdiff()` (original decoder's bit reader) | Unaligned `*(u16*)&data[ix]` loads with no bound. The original scan loop never checks its position, so a damaged or cut stream read far past the buffer. | `lj92_next16()`: the two bytes, zeros past the end, as the fast decoder reads. |
| `find()` | Read `data[ix]` before checking `ix`. After a segment's length `ix` can be up to 64 KB past the data. | The bound is checked first. |
| `parseSof3()` | Checked `ix + 6` but read `ix + 7` (the component count). A bit depth of 0 or over 16 let the decoders compute `1 << (bits - 1)` out of range. | Checks `ix + 7`. A bit depth outside 2 to 16 is corrupt. |
| `parseBlock()` | Read the 2-byte segment length with only its first byte checked. | Both bytes must be in the data. |
| `parseScan()` | Read the scan header's component count and the predictor after it (up to 513 bytes on) unchecked. | Both must be in the data. |
| `parseHuff()` | No check that the 19-byte header is in the data. The values were read for as many codes as the 16 counts add up to, whatever the segment's length: past the segment, and past the buffer at the end. Values over 16 (not a size category) made the decoders shift by up to 255 bits. | The header must be in the data, the counts must fit the segment, and every value must be 0 to 16. |
| `parseHuff()`, lookup table | `malloc`: entries an incomplete code leaves unfilled were garbage. A second table segment replaced the first without freeing it, leaking it on every frame decoded. | `calloc` (unfilled entries read as 0); a second table frees the first. |
| `lj92_open()` | A stream without a Huffman table (DHT) or frame header (SOF3) was accepted. The fast decoder then built its table from a null pointer: a crash. Found by fuzzing (see below). | Corrupt without a table, or with no width, height or components. |
| `frequencyScan()`, `writeBody()` (original encoder) | Their row buffers were used without checking the allocation. | `LJ92_ERROR_NO_MEMORY`. |
| `lj92_encode()`, both paths | `encoded = realloc(encoded, written)` lost the buffer (a leak, and no result) if the shrink failed. | A failed shrink keeps the buffer as it is. |

Left as it was: `parseHuff()` writes a 0 into the caller's data (`bits[0] = 0`, the byte before
the 16 counts), as the original does; `lj92_open()` takes non-const data. Damaged data that passes
these checks decodes to wrong values, but always inside the image and the data.

## How it was checked

- **Same results.** `mlvdrive_tool selftest` (run by `tools/test.ps1` on one clip of each kind)
  decodes every frame with the fast and the original decoder. It also encodes every 32nd frame
  with the fast and the original encoder, and compares the bytes. Test set: 5D Mark III LJ92
  1080p and 3K, uncompressed 5D Mark III and 60D (encoder only); all equal.
- **Damaged data.** A fuzzing harness (not part of the project) was built with MSVC's
  AddressSanitizer. It reads frames of `E:\mlv\M21-1642.MLV` through `mlv::Clip`, plus a frame
  from MLV-App's encoder (predictor 6). It then decodes 3000 damaged copies with both decoders:
  cut anywhere, cut inside the headers, header bytes changed, bytes changed anywhere. Each copy is
  in a buffer of exactly its size, so any read past it is reported.
  - The code as of commit 5b7c409 (the row-buffer fix only) overflowed the heap on the first
    input: a cut camera frame, in `nextdiff()`.
  - With the bounds fixed, it found the missing-table crash in `lj92_open()`.
  - The final code: 3000 inputs, 6000 decodes, no memory errors.
- Not checked: a `SLOW_HUFF` build (the original's other Huffman decoder, not used here).
