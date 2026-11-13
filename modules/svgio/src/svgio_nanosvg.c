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
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/gc.h"

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
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// --- svgio's use of it -------------------------------------------------------

svgio_image_t *svgio_parse(char *text, float *width, float *height) {
    NSVGimage *img = nsvgParse(text, "px", 96.0f);
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

const char *svgio_render(svgio_image_t *image, float scale, int width, int height,
    uint8_t *strip, int strip_rows, svgio_rows_fn rows, void *ctx) {
    NSVGrasterizer *r = nsvgCreateRasterizer();
    if (r == NULL) {
        return "not enough memory to render";
    }
    const char *why = NULL;
    for (int y = 0; y < height && why == NULL; y += strip_rows) {
        int n = height - y < strip_rows ? height - y : strip_rows;
        // A strip is the picture moved up by y and cut to n rows.
        nsvgRasterize(r, (NSVGimage *)image, 0.0f, (float)-y, scale, strip, width, n, width * 4);
        if (r->scanline == NULL) {
            why = "not enough memory to render";
            break;
        }
        rows(ctx, strip, y, n, width);
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
