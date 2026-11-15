// svgio's engine: NanoSVG's parser and rasteriser (svgio_nanosvg.c), shared
// by the Python module (mod_svgio.c) and the LVGL image decoder
// (lvgl_decoder.c).
//
// SPDX-License-Identifier: MIT

#ifndef SVGIO_CORE_H
#define SVGIO_CORE_H

#include <stdbool.h>
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

// The compact form (svgio_compact.c): a drawing as records of numbers. True
// when src starts as one does.
bool svgio_is_compact(const uint8_t *src, size_t len);
// The SVG text it stands for, NUL-terminated, from svgio_mem_alloc(), and its
// length in *out_len; or NULL and why not.
char *svgio_expand(const uint8_t *src, size_t len, size_t *out_len, const char **why);

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

// Render rows y0 up to y1 of image at scale, width x height pixels in all,
// strip_rows rows at a time through strip (width * strip_rows * 4 bytes),
// handing each strip to rows(). The strips sit where a render of every row
// puts them, so a picture drawn in bands is the picture drawn whole.
// prepared is the image prepared at that scale. swaps is nswaps pairs of
// colours, each 0xBBGGRR as NanoSVG holds them: wherever a fill, a stroke or
// a gradient's stop is the first of a pair, the second is drawn, with the
// opacity the first had. NULL, or why not.
const char *svgio_render(svgio_image_t *image, svgio_prepared_t *prepared, float scale, int width, int height,
    int y0, int y1, const uint32_t *swaps, int nswaps, uint8_t *strip, int strip_rows, svgio_rows_fn rows, void *ctx);

// The most pairs of colours a render swaps.
#define SVGIO_MAX_SWAPS 128

// n RGBA pixels to native-order RGB565. With alpha, pixels gets the colour
// and alpha its coverage (pixels may be NULL for the mask alone). Without
// alpha, the colour is blended over what pixels already holds.
void svgio_rgba_to_565(const uint8_t *rgba, size_t n, uint16_t *pixels, uint8_t *alpha);

// A profiling build: SVGIO_BENCH=1 times each phase and lets svgio._opts()
// switch each speedup off (bit 1: the scanline's fast paths, 2: rows with no
// edges skipped, 4: prepared outlines, 8: no divide at full alpha, 16: closing
// segments of no length not split).
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

// Memory from outside the MicroPython heap, where the port has some: the
// collector never reads it, so megabytes of it cost a collection nothing. It
// isn't cleared, and whoever takes it gives it back. NULL when there is none
// or not that much. Ports without any always give NULL.
void *svgio_outside_alloc(size_t n);
void svgio_outside_free(void *p);
// How many blocks from there are held now.
size_t svgio_outside_held(void);

// A decoder's working memory. With size set, base is one block that the
// parser and the rasteriser take their memory from, a small allocator of its
// own inside it, so an open() or a decode() asks the heap for nothing. What
// doesn't fit spills: to the MicroPython heap when base is a block of it, and
// outside it when base is (outside), since the collector can't see into base
// to find what would be kept. With size 0 it only counts what its decoder
// holds.
typedef struct {
    uint8_t *base;          // the block's head, which keeps it alive; NULL without one
    bool outside;           // base came from svgio_outside_alloc()
    void *spilled;          // the blocks that spilled outside, a list
    uint32_t size;          // bytes of it, a multiple of 8
    uint32_t rover;         // where the next search for room starts
    size_t used;            // bytes of base in use, block headers included
    size_t spill;           // bytes on the heap instead
    size_t peak;            // the most used + spill since svgio_arena_reset()
} svgio_arena_t;

// Everything in the arena is forgotten, and what spilled outside is given
// back. Free what spilled to the heap first.
void svgio_arena_reset(svgio_arena_t *arena);

// Whose memory the calls below, the parser and the rasteriser use until the
// next svgio_mem_use(): an arena, or NULL for the heap alone. A block goes
// back with the arena it came from in use.
void svgio_mem_use(svgio_arena_t *arena);

// Memory from the arena in use, or the MicroPython heap: NULL when it runs out.
void *svgio_mem_alloc(size_t n);
void svgio_mem_free(void *p);

// Where the LVGL decoder keeps the last SVG it read: a root pointer, so the
// garbage collector sees what it holds.
void **svgio_kept_slot(void);

// The most heap NanoSVG has held at once since svgio_mem_reset().
void svgio_mem_reset(void);
size_t svgio_mem_peak(void);

#endif
