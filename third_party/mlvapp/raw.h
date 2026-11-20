/*
 * Portable replacement for MLV-App's src/mlv/raw.h (Magic Lantern, GPL-2.0-or-later).
 *
 * The original declares bit-field pixel blocks with __attribute__((packed)), which MSVC can't
 * compile. The vendored code (llrawproc/pixelproc.c) only needs the header to exist and the
 * fixed-width integer types; the raw_info layout lives in src/mlv/mlv_format.h.
 */
#ifndef _raw_h_
#define _raw_h_

#include <stdint.h>

#endif
