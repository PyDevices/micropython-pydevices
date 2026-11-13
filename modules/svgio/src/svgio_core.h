// svgio's engine: NanoSVG's parser and rasteriser (svgio_nanosvg.c), shared
// by the Python module (mod_svgio.c) and the LVGL image decoder
// (lvgl_decoder.c).
//
// SPDX-License-Identifier: MIT

#ifndef SVGIO_CORE_H
#define SVGIO_CORE_H

#include <stddef.h>
#include <stdint.h>

// A parsed SVG: NanoSVG's NSVGimage, kept opaque here.
typedef struct svgio_image svgio_image_t;

// Rows of RGBA bytes, straight (not premultiplied) alpha.
typedef void (*svgio_rows_fn)(void *ctx, const uint8_t *rgba, int y, int rows, int width);

// The largest strip of RGBA a render holds at once, in bytes. A taller
// picture renders in strips of as many rows as fit, which costs a pass over
// the outlines per strip.
#define SVGIO_STRIP_BYTES (64 * 1024)

// Parse NUL-terminated SVG text, which the parser changes. NULL when the heap
// runs out; otherwise the image, and its size in pixels at 96 dpi.
svgio_image_t *svgio_parse(char *text, float *width, float *height);
void svgio_image_free(svgio_image_t *image);

// Render image at scale into width x height pixels, strip_rows rows at a time
// through strip (width * strip_rows * 4 bytes), handing each strip to rows().
// NULL, or why not.
const char *svgio_render(svgio_image_t *image, float scale, int width, int height,
    uint8_t *strip, int strip_rows, svgio_rows_fn rows, void *ctx);

// n RGBA pixels to native-order RGB565. With alpha, pixels gets the colour
// and alpha its coverage (pixels may be NULL for the mask alone). Without
// alpha, the colour is blended over what pixels already holds.
void svgio_rgba_to_565(const uint8_t *rgba, size_t n, uint16_t *pixels, uint8_t *alpha);

// The most heap NanoSVG has held at once since svgio_mem_reset().
void svgio_mem_reset(void);
size_t svgio_mem_peak(void);

#endif
