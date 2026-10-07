// pngio's engine: PNG encode (filters and deflate) and the decode side's
// parsing, unfiltering and conversion, in portable C with no MicroPython in
// it. The binding (mod_pngio.c) allocates, and inflates through
// MicroPython's uzlib.
//
// The encoder writes one zlib stream in a single fixed-Huffman deflate block.
// Matches come from a hash of the next three bytes, with a chain of earlier
// positions searched as deep as the level asks (level 1: the newest only). Each
// row takes the filter (None, Sub, Up or Paeth) whose output has the smallest
// sum of absolute values, the usual heuristic.
//
// SPDX-License-Identifier: MIT

#ifndef PNGIO_CORE_H
#define PNGIO_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pixel formats in, and what they become in the PNG.
enum {
    PNGIO_RGB565 = 0,   // native-order 16-bit; widened by bit replication (31 -> 255)
    PNGIO_GS8 = 1,      // one byte of grey
    PNGIO_RGB888 = 2,   // R, G, B bytes
};

// Bytes of input per pixel, and channels in the PNG.
int pngio_in_bpp(int fmt);
int pngio_channels(int fmt);

// The most bytes a w x h image can encode to, and the scratch encode needs.
size_t pngio_encode_bound(uint32_t w, uint32_t h, int fmt);
size_t pngio_encode_work(uint32_t w, uint32_t h, int fmt, int level);

// Encode. `stride` is in pixels (0: w). `swap`: RGB565 is big-endian.
// `level` 1..9. NULL when done, with *out_len set; else why not.
const char *pngio_encode(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, int fmt,
    bool swap, int level, void *work, uint8_t *out, size_t cap, size_t *out_len);

// --- decoding ----------------------------------------------------------------

typedef struct {
    uint32_t width, height;
    uint8_t depth, ctype, interlace;
    uint16_t npalette;
    uint8_t palette[256 * 3];
    int bpp;                // bytes per complete pixel, at least 1 (for the filters)
    size_t rowbytes;        // bytes of one scanline, without its filter byte
} pngio_info_t;

// Read the signature, IHDR and PLTE. NULL when the image can be decoded
// (8-bit gray, RGB, palette, gray+alpha, RGBA; 1/2/4-bit gray and palette;
// not interlaced), else why not.
const char *pngio_parse(const uint8_t *png, size_t len, pngio_info_t *info);

// Walk the IDAT chunks: *pos starts at 0; true with the next chunk's payload,
// false after the last.
bool pngio_next_idat(const uint8_t *png, size_t len, size_t *pos, const uint8_t **data, size_t *dlen);

// Undo one row's filter in place (prev is the previous unfiltered row, or
// zeros for the first). NULL, or why the filter type is bad.
const char *pngio_unfilter(uint8_t ftype, uint8_t *row, const uint8_t *prev, size_t rowbytes, int bpp);

// One unfiltered row to native-order RGB565 (alpha is dropped).
void pngio_row_to_565(const pngio_info_t *info, const uint8_t *row, uint16_t *dst, uint32_t w);

#endif
