// NanoSVG's parser and rasteriser, compiled for MicroPython.
//
// The headers in third_party/nanosvg are upstream's, with one fix that its
// ORIGIN.txt describes. They call the C library for memory, number parsing,
// colour parsing and sorting; the macros below point those calls at svgio's
// own versions before the headers are included, so every port links the same
// code:
//
// - memory comes from the MicroPython heap, so a parse that runs out returns
//   NULL (NanoSVG then frees what it had) instead of failing in a C library
//   allocator the port may not have, and the garbage collector sees every
//   block NanoSVG holds;
// - sscanf, strtoll, strtol and qsort are small local versions, because
//   bare-metal ports don't all link a C library that has them;
// - the two double-precision calls, pow and sqrt, go through single-precision
//   maths, which every port with floats has.
//
// SPDX-License-Identifier: MIT

#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/mphal.h"

#include "svgio_core.h"

// --- memory ------------------------------------------------------------------

static size_t svgio_held, svgio_peak;

static void svgio_count(void *p, int sign) {
    if (p != NULL) {
        size_t n = gc_nbytes(p);
        svgio_held = sign > 0 ? svgio_held + n : svgio_held - n;
        if (svgio_held > svgio_peak) {
            svgio_peak = svgio_held;
        }
    }
}

static void *svgio_malloc(size_t n) {
    void *p = n ? gc_alloc(n, 0) : NULL;
    svgio_count(p, 1);
    return p;
}

static void svgio_free(void *p) {
    svgio_count(p, -1);
    gc_free(p);
}

static void *svgio_realloc(void *p, size_t n) {
    if (p == NULL) {
        return svgio_malloc(n);
    }
    if (n == 0) {
        svgio_free(p);
        return NULL;
    }
    size_t was = gc_nbytes(p);
    void *q = gc_realloc(p, n, true);
    if (q != NULL) {
        svgio_held -= was;
        svgio_count(q, 1);
    }
    return q;
}

void *svgio_mem_alloc(size_t n) {
    return svgio_malloc(n);
}

void svgio_mem_free(void *p) {
    if (p != NULL) {
        svgio_free(p);
    }
}

void svgio_mem_reset(void) {
    svgio_peak = svgio_held;
}

size_t svgio_mem_peak(void) {
    return svgio_peak;
}

// --- numbers -----------------------------------------------------------------

static long long svgio_strtoll(const char *s, char **end, int base) {
    (void)base;     // NanoSVG only parses decimal
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = *p++ == '-';
    }
    if (*p < '0' || *p > '9') {
        if (end != NULL) {
            *end = (char *)s;
        }
        return 0;
    }
    unsigned long long v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p++ - '0');
    }
    if (end != NULL) {
        *end = (char *)p;
    }
    return neg ? -(long long)v : (long long)v;
}

static long svgio_strtol(const char *s, char **end, int base) {
    return (long)svgio_strtoll(s, end, base);
}

// NanoSVG raises only 10 to a whole power.
static double svgio_pow(double base, double exp) {
    float r = 1.0f, b = (float)base;
    int e = (int)exp;
    if (e < 0) {
        b = 1.0f / b;
        e = -e;
    }
    for (; e > 0; e >>= 1) {
        if (e & 1) {
            r *= b;
        }
        b *= b;
    }
    return (double)r;
}

static double svgio_sqrt(double x) {
    return (double)sqrtf((float)x);
}

// --- colours -----------------------------------------------------------------

// The three formats NanoSVG passes: "#%2x%2x%2x", "#%1x%1x%1x" and
// "rgb(%u, %u, %u)". A space in the format matches any run of white space,
// %u and %Nx skip white space first, and every conversion stores an unsigned
// int. Returns the number stored, stopping at the first mismatch.
static int svgio_hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static int svgio_isspace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int svgio_sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int stored = 0;
    while (*fmt) {
        if (svgio_isspace(*fmt)) {
            while (svgio_isspace(*s)) {
                s++;
            }
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) {
                break;
            }
            s++;
            fmt++;
            continue;
        }
        fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        char conv = *fmt++;
        int radix = conv == 'x' ? 16 : 10;
        while (svgio_isspace(*s)) {
            s++;
        }
        unsigned v = 0;
        int n = 0, d;
        while ((width == 0 || n < width) && (d = svgio_hex(*s)) >= 0 && d < radix) {
            v = v * radix + d;
            s++;
            n++;
        }
        if (n == 0) {
            break;
        }
        *va_arg(ap, unsigned *) = v;
        stored++;
    }
    va_end(ap);
    return stored;
}

// --- sorting -----------------------------------------------------------------

// A heap sort: no recursion and no scratch memory, and NanoSVG sorts only
// the edges of one shape at a time.
static void svgio_swap(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void svgio_sift(char *base, size_t root, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t child = 2 * root + 1;
        if (child >= n) {
            return;
        }
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) {
            child++;
        }
        if (cmp(base + root * size, base + child * size) >= 0) {
            return;
        }
        svgio_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

static void svgio_qsort(void *items, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *base = items;
    if (n < 2) {
        return;
    }
    for (size_t i = n / 2; i-- > 0;) {
        svgio_sift(base, i, n, size, cmp);
    }
    for (size_t end = n - 1; end > 0; end--) {
        svgio_swap(base, base + end * size, size);
        svgio_sift(base, 0, end, size, cmp);
    }
}

// --- NanoSVG -----------------------------------------------------------------

#define malloc svgio_malloc
#define realloc svgio_realloc
#define free svgio_free
#define strtoll svgio_strtoll
#define strtol svgio_strtol
#define sscanf svgio_sscanf
#define qsort svgio_qsort
#define pow svgio_pow
#define sqrt svgio_sqrt
// nsvgParseFromFile reads with stdio, which svgio never calls: a file that
// never opens leaves nothing of it to link.
static FILE *svgio_fopen(const char *name, const char *mode) {
    (void)name;
    (void)mode;
    return NULL;
}
static int svgio_fclose(FILE *fp) {
    (void)fp;
    return 0;
}
static int svgio_fseek(FILE *fp, long off, int whence) {
    (void)fp;
    (void)off;
    (void)whence;
    return -1;
}
static long svgio_ftell(FILE *fp) {
    (void)fp;
    return -1;
}
static size_t svgio_fread(void *buf, size_t size, size_t n, FILE *fp) {
    (void)buf;
    (void)size;
    (void)n;
    (void)fp;
    return 0;
}
#define fopen svgio_fopen
#define fclose svgio_fclose
#define fseek svgio_fseek
#define ftell svgio_ftell
#define fread svgio_fread

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-declarations"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

// Nested groups the parser tracks. Each level holds about 300 bytes for the
// whole parse, so upstream's 128 cost 41 KB even for a 24 px icon. Deeper
// groups than this lose track of their styles.
#define NSVG_MAX_ATTR 32

// A profiling build (SVGIO_BENCH=1) can switch each speedup off at run time
// and times each phase, through svgio._opts() and svgio._profile().
#if SVGIO_BENCH
unsigned svgio_opts = ~0u;
uint32_t svgio_phase_us[SVGIO_PHASES];
#define SVGIO_OPT(bit) (svgio_opts & (bit))
static mp_uint_t svgio_t0;
#define SVGIO_TIME_START() (svgio_t0 = mp_hal_ticks_us())
#define SVGIO_TIME_END(phase) (svgio_phase_us[phase] += mp_hal_ticks_us() - svgio_t0)
#else
#define SVGIO_TIME_START() do {} while (0)
#define SVGIO_TIME_END(phase) do {} while (0)
#endif
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// --- svgio's use of it -------------------------------------------------------

svgio_image_t *svgio_parse(char *text, float *width, float *height) {
    SVGIO_TIME_START();
    NSVGimage *img = nsvgParse(text, "px", 96.0f);
    SVGIO_TIME_END(SVGIO_PHASE_PARSE);
    if (img != NULL) {
        *width = img->width;
        *height = img->height;
    }
    return (svgio_image_t *)img;
}

void svgio_image_free(svgio_image_t *image) {
    if (image != NULL) {
        nsvgDelete((NSVGimage *)image);
    }
}

// One shape's fill or stroke, flattened to edges at the output scale. The
// edges are sorted by their top and not yet moved to a strip, so every strip
// and every later decode() at this scale uses them as they are; edges is NULL
// when they didn't fit SVGIO_KEEP_BYTES and are flattened again per strip.
typedef struct {
    NSVGshape *shape;
    NSVGedge *edges;
    int nedges;
    bool stroke;
    float top, bottom;      // the rows, in output pixels, the edges span
} svgio_pass_t;

struct svgio_prepared {
    float scale;
    int npasses;
    svgio_pass_t pass[];
};

// The fills and strokes NanoSVG paints, in its order: into pass when it isn't
// NULL, and how many either way.
static int svgio_passes(NSVGimage *image, float scale, svgio_pass_t *pass) {
    int n = 0;
    for (NSVGshape *shape = image->shapes; shape != NULL; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) {
            continue;
        }
        for (int j = 0; j < 3; j++) {
            int order = (shape->paintOrder >> (2 * j)) & 0x03;
            bool fill = order == NSVG_PAINT_FILL && shape->fill.type != NSVG_PAINT_NONE;
            bool stroke = order == NSVG_PAINT_STROKE && shape->stroke.type != NSVG_PAINT_NONE
                && shape->strokeWidth * scale > 0.01f;
            if (fill || stroke) {
                if (pass != NULL) {
                    pass[n] = (svgio_pass_t) { shape, NULL, 0, stroke, 0.0f, 0.0f };
                }
                n++;
            }
        }
    }
    return n;
}

// The pass's edges into r->edges, unmoved and sorted. False when the heap ran out.
static bool svgio_flatten(NSVGrasterizer *r, NSVGshape *shape, bool stroke, float scale) {
    nsvg__resetPool(r);
    r->freelist = NULL;
    r->nedges = 0;
    if (stroke) {
        nsvg__flattenShapeStroke(r, shape, scale);
    } else {
        nsvg__flattenShape(r, shape, scale);
    }
    if (r->edges == NULL && r->cedges > 0) {
        return false;
    }
    if (r->nedges != 0) {
        qsort(r->edges, r->nedges, sizeof(NSVGedge), nsvg__cmpEdge);
    }
    return true;
}

svgio_prepared_t *svgio_prepare(svgio_image_t *image_in, float scale) {
    NSVGimage *image = (NSVGimage *)image_in;
    int n = svgio_passes(image, scale, NULL);
    svgio_prepared_t *p = malloc(sizeof(svgio_prepared_t) + n * sizeof(svgio_pass_t));
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (p == NULL || r == NULL) {
        free(p);
        if (r != NULL) {
            nsvgDeleteRasterizer(r);
        }
        return NULL;
    }
    SVGIO_TIME_START();
    p->scale = scale;
    p->npasses = svgio_passes(image, scale, p->pass);
    size_t kept = 0;
    bool ok = true;
    for (int k = 0; k < p->npasses && ok; k++) {
        svgio_pass_t *ps = &p->pass[k];
        ok = svgio_flatten(r, ps->shape, ps->stroke, scale);
        if (!ok || r->nedges == 0) {
            continue;
        }
        ps->nedges = r->nedges;
        ps->top = r->edges[0].y0;
        ps->bottom = ps->top;
        for (int i = 0; i < r->nedges; i++) {
            ps->bottom = r->edges[i].y1 > ps->bottom ? r->edges[i].y1 : ps->bottom;
        }
        size_t bytes = r->nedges * sizeof(NSVGedge);
        if (kept + bytes <= SVGIO_KEEP_BYTES) {
            ps->edges = malloc(bytes);
            ok = ps->edges != NULL;
            if (ok) {
                memcpy(ps->edges, r->edges, bytes);
                kept += bytes;
            }
        }
    }
    nsvgDeleteRasterizer(r);
    SVGIO_TIME_END(SVGIO_PHASE_PREPARE);
    if (!ok) {
        svgio_prepared_free(p);
        return NULL;
    }
    return p;
}

void svgio_prepared_free(svgio_prepared_t *p) {
    if (p != NULL) {
        for (int i = 0; i < p->npasses; i++) {
            free(p->pass[i].edges);
        }
        free(p);
    }
}

float svgio_prepared_scale(const svgio_prepared_t *p) {
    return p->scale;
}

// One strip, rows -ty up, of every pass that reaches it: NanoSVG's
// nsvgRasterize, with the flattening done once in svgio_prepare.
static const char *svgio_strip(NSVGrasterizer *r, svgio_prepared_t *p,
    float scale, float ty, uint8_t *dst, int w, int h) {
    r->bitmap = dst;
    r->width = w;
    r->height = h;
    r->stride = w * 4;
    memset(dst, 0, (size_t)w * h * 4);
    for (int k = 0; k < p->npasses; k++) {
        svgio_pass_t *ps = &p->pass[k];
        if (ps->nedges == 0 || (ty + ps->bottom) * NSVG__SUBSAMPLES <= 0
            || (ty + ps->top) * NSVG__SUBSAMPLES >= h * NSVG__SUBSAMPLES) {
            continue;       // nothing of it in this strip
        }
        SVGIO_TIME_START();
        if (ps->edges != NULL) {
            if (r->cedges < ps->nedges) {
                NSVGedge *e = realloc(r->edges, sizeof(NSVGedge) * ps->nedges);
                if (e == NULL) {
                    return "not enough memory to render";
                }
                r->edges = e;
                r->cedges = ps->nedges;
            }
            memcpy(r->edges, ps->edges, sizeof(NSVGedge) * ps->nedges);
            r->nedges = ps->nedges;
            nsvg__resetPool(r);
            r->freelist = NULL;
        } else if (!svgio_flatten(r, ps->shape, ps->stroke, scale)) {
            return "not enough memory to render";
        }
        SVGIO_TIME_END(SVGIO_PHASE_EDGES);
        SVGIO_TIME_START();
        // Moved to the strip, in subsamples, as nsvgRasterize does.
        for (int i = 0; i < r->nedges; i++) {
            NSVGedge *e = &r->edges[i];
            e->y0 = (ty + e->y0) * NSVG__SUBSAMPLES;
            e->y1 = (ty + e->y1) * NSVG__SUBSAMPLES;
        }
        NSVGcachedPaint cache;
        nsvg__initPaint(&cache, ps->stroke ? &ps->shape->stroke : &ps->shape->fill, ps->shape->opacity);
        nsvg__rasterizeSortedEdges(r, 0.0f, ty, scale, &cache, ps->stroke ? NSVG_FILLRULE_NONZERO : ps->shape->fillRule);
        SVGIO_TIME_END(SVGIO_PHASE_RASTER);
    }
    SVGIO_TIME_START();
    nsvg__unpremultiplyAlpha(dst, w, h, w * 4);
    SVGIO_TIME_END(SVGIO_PHASE_UNPREMULTIPLY);
    return NULL;
}

const char *svgio_render(svgio_image_t *image, svgio_prepared_t *prepared, float scale, int width, int height,
    uint8_t *strip, int strip_rows, svgio_rows_fn rows, void *ctx) {
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (r == NULL) {
        return "not enough memory to render";
    }
    r->scanline = malloc(width);
    r->cscanline = width;
    const char *why = r->scanline == NULL ? "not enough memory to render" : NULL;
    for (int y = 0; y < height && why == NULL; y += strip_rows) {
        int n = height - y < strip_rows ? height - y : strip_rows;
        // A strip is the picture moved up by y and cut to n rows.
        if (prepared != NULL && SVGIO_OPT(4)) {
            why = svgio_strip(r, prepared, scale, (float)-y, strip, width, n);
        } else {
            SVGIO_TIME_START();
            nsvgRasterize(r, (NSVGimage *)image, 0.0f, (float)-y, scale, strip, width, n, width * 4);
            SVGIO_TIME_END(SVGIO_PHASE_RASTER);
            if (r->scanline == NULL) {
                why = "not enough memory to render";
            }
        }
        if (why == NULL) {
            SVGIO_TIME_START();
            rows(ctx, strip, y, n, width);
            SVGIO_TIME_END(SVGIO_PHASE_CONVERT);
        }
    }
    nsvgDeleteRasterizer(r);
    return why;
}

// --- RGBA to RGB565 ----------------------------------------------------------

static inline uint16_t svgio_565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void svgio_rgba_to_565(const uint8_t *rgba, size_t n, uint16_t *pixels, uint8_t *alpha) {
    for (size_t i = 0; i < n; i++, rgba += 4) {
        unsigned a = rgba[3];
        if (alpha != NULL) {
            alpha[i] = (uint8_t)a;
            if (pixels != NULL) {
                pixels[i] = svgio_565(rgba[0], rgba[1], rgba[2]);
            }
            continue;
        }
        if (a == 0) {
            continue;
        }
        if (a == 255) {
            pixels[i] = svgio_565(rgba[0], rgba[1], rgba[2]);
            continue;
        }
        // Over what the buffer already holds, widened to 8 bits a channel.
        unsigned bg = pixels[i];
        unsigned br = (bg >> 8) & 0xF8, bgn = (bg >> 3) & 0xFC, bb = (bg << 3) & 0xF8;
        br |= br >> 5;
        bgn |= bgn >> 6;
        bb |= bb >> 5;
        unsigned ia = 255 - a;
        pixels[i] = svgio_565((rgba[0] * a + br * ia + 127) / 255,
            (rgba[1] * a + bgn * ia + 127) / 255,
            (rgba[2] * a + bb * ia + 127) / 255);
    }
}
