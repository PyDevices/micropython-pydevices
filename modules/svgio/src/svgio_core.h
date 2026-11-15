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

// The most a prepared image keeps of its flattened outlines, in bytes. Past
// it, the rest are flattened again for each strip.
#define SVGIO_KEEP_BYTES (64 * 1024)

// An image's outlines flattened at one scale, ready to render any number of
// times: svgio_prepare() does the flattening a render would otherwise repeat
// for every strip. NULL when the heap runs out.
typedef struct svgio_prepared svgio_prepared_t;
svgio_prepared_t *svgio_prepare(svgio_image_t *image, float scale);
void svgio_prepared_free(svgio_prepared_t *prepared);
float svgio_prepared_scale(const svgio_prepared_t *prepared);

// Render image at scale into width x height pixels, strip_rows rows at a time
// through strip (width * strip_rows * 4 bytes), handing each strip to rows().
// prepared is the image prepared at that scale. NULL, or why not.
const char *svgio_render(svgio_image_t *image, svgio_prepared_t *prepared, float scale, int width, int height,
    uint8_t *strip, int strip_rows, svgio_rows_fn rows, void *ctx);

// n RGBA pixels to native-order RGB565. With alpha, pixels gets the colour
// and alpha its coverage (pixels may be NULL for the mask alone). Without
// alpha, the colour is blended over what pixels already holds.
void svgio_rgba_to_565(const uint8_t *rgba, size_t n, uint16_t *pixels, uint8_t *alpha);

// A profiling build: SVGIO_BENCH=1 times each phase and lets svgio._opts()
// switch each speedup off (bit 1: the scanline's fast paths, 2: rows with no
// edges skipped, 4: prepared outlines, 8: no divide at full alpha).
#ifndef SVGIO_BENCH
#define SVGIO_BENCH 0
#endif
enum {
    SVGIO_PHASE_PARSE, SVGIO_PHASE_PREPARE, SVGIO_PHASE_EDGES, SVGIO_PHASE_RASTER,
    SVGIO_PHASE_UNPREMULTIPLY, SVGIO_PHASE_CONVERT, SVGIO_PHASES
};
#if SVGIO_BENCH
extern unsigned svgio_opts;
extern uint32_t svgio_phase_us[SVGIO_PHASES];
#endif

// Memory on the MicroPython heap, as NanoSVG's is: NULL when it runs out.
void *svgio_mem_alloc(size_t n);
void svgio_mem_free(void *p);

// Where the LVGL decoder keeps the last SVG it read: a root pointer, so the
// garbage collector sees what it holds.
void **svgio_kept_slot(void);

// The most heap NanoSVG has held at once since svgio_mem_reset().
void svgio_mem_reset(void);
size_t svgio_mem_peak(void);

#endif
