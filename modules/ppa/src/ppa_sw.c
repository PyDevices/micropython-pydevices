// ppa's software path: scale, rotate, mirror and colour convert in portable
// C, for every port (and on the P4 when asked: the reference its PPA is
// checked against). Fill and blend are the PPA's alone (D5).
//
// Each operation reads its source block into an RGB888 working picture the
// size of the destination block, nearest-neighbour, then writes that in the
// destination format: per pixel for RGB and gray, per pixel pair for YUY2
// and UYVY, per 2x2 block for YUV420 (chroma from the average).

#include <stdlib.h>
#include <string.h>

#include "ppa_mod.h"
#include "ppa_sw.h"

size_t ppa_mod_size(int fmt, uint32_t w, uint32_t h) {
    size_t n = (size_t)w * h;
    switch (fmt) {
        case PPA_MOD_RGB565:
        case PPA_MOD_YUY2:
        case PPA_MOD_UYVY:
            return n * 2;
        case PPA_MOD_RGB888:
            return n * 3;
        case PPA_MOD_ARGB8888:
            return n * 4;
        case PPA_MOD_GRAY8:
            return n;
        case PPA_MOD_YUV420:
            return n * 3 / 2;
    }
    return 0;
}

static inline uint8_t clamp8(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

// --- YUV <-> RGB, BT.601 ------------------------------------------------------

static void yuv_to_rgb(int y, int u, int v, bool limited, uint8_t *rgb) {
    int d = u - 128, e = v - 128;
    if (limited) {
        int c = y - 16;
        rgb[0] = clamp8((298 * c + 409 * e + 128) >> 8);
        rgb[1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
        rgb[2] = clamp8((298 * c + 516 * d + 128) >> 8);
    } else {
        rgb[0] = clamp8(y + ((91881 * e + 32768) >> 16));
        rgb[1] = clamp8(y - ((22554 * d + 46802 * e - 32768) >> 16));
        rgb[2] = clamp8(y + ((116130 * d + 32768) >> 16));
    }
}

static uint8_t rgb_to_y(const uint8_t *p, bool limited) {
    if (limited) {
        return clamp8(((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8) + 16);
    }
    return clamp8((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
}

static void rgb_to_uv(int r, int g, int b, bool limited, uint8_t *u, uint8_t *v) {
    if (limited) {
        *u = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        *v = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    } else {
        *u = clamp8(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
        *v = clamp8(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
    }
}

// --- reading one source pixel as R, G, B ----------------------------------------

static void read_px(const uint8_t *src, int fmt, uint32_t w, uint32_t x, uint32_t y, bool swap, bool limited, uint8_t *rgb) {
    switch (fmt) {
        case PPA_MOD_RGB565: {
            const uint8_t *p = src + ((size_t)y * w + x) * 2;
            unsigned v = swap ? ((unsigned)p[0] << 8 | p[1]) : ((unsigned)p[1] << 8 | p[0]);
            unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            rgb[0] = (uint8_t)((r << 3) | (r >> 2));
            rgb[1] = (uint8_t)((g << 2) | (g >> 4));
            rgb[2] = (uint8_t)((b << 3) | (b >> 2));
            return;
        }
        case PPA_MOD_RGB888: {
            const uint8_t *p = src + ((size_t)y * w + x) * 3;
            rgb[0] = p[2];
            rgb[1] = p[1];
            rgb[2] = p[0];
            return;
        }
        case PPA_MOD_ARGB8888: {
            const uint8_t *p = src + ((size_t)y * w + x) * 4;
            rgb[0] = p[2];
            rgb[1] = p[1];
            rgb[2] = p[0];
            return;
        }
        case PPA_MOD_GRAY8:
            rgb[0] = rgb[1] = rgb[2] = src[(size_t)y * w + x];
            return;
        case PPA_MOD_YUY2:
        case PPA_MOD_UYVY: {
            const uint8_t *p = src + ((size_t)y * w + (x & ~1u)) * 2;
            int yy, u, v;
            if (fmt == PPA_MOD_YUY2) {
                yy = p[(x & 1) ? 2 : 0];
                u = p[1];
                v = p[3];
            } else {
                yy = p[(x & 1) ? 3 : 1];
                u = p[0];
                v = p[2];
            }
            yuv_to_rgb(yy, u, v, limited, rgb);
            return;
        }
        case PPA_MOD_YUV420: {
            size_t line = (size_t)w * 3 / 2;
            const uint8_t *l0 = src + (size_t)(y & ~1u) * line + (x >> 1) * 3;
            const uint8_t *l1 = l0 + line;
            int yy = ((y & 1) ? l1 : l0)[1 + (x & 1)];
            yuv_to_rgb(yy, l0[0], l1[0], limited, rgb);
            return;
        }
    }
    rgb[0] = rgb[1] = rgb[2] = 0;
}

// --- writing the working picture in the destination format ----------------------

static void write_rgb(const uint8_t *work, uint32_t bw, uint32_t bh, const ppa_mod_srm_t *op) {
    uint8_t *dst = op->dst;
    for (uint32_t j = 0; j < bh; j++) {
        const uint8_t *s = work + (size_t)j * bw * 3;
        uint32_t row = op->y + j;
        for (uint32_t i = 0; i < bw; i++, s += 3) {
            size_t at = (size_t)row * op->dst_w + op->x + i;
            switch (op->dst_fmt) {
                case PPA_MOD_RGB565: {
                    unsigned v = ((unsigned)(s[0] & 0xF8) << 8) | ((unsigned)(s[1] & 0xFC) << 3) | (s[2] >> 3);
                    dst[at * 2] = (uint8_t)v;
                    dst[at * 2 + 1] = (uint8_t)(v >> 8);
                    break;
                }
                case PPA_MOD_RGB888:
                    dst[at * 3] = s[2];
                    dst[at * 3 + 1] = s[1];
                    dst[at * 3 + 2] = s[0];
                    break;
                case PPA_MOD_ARGB8888:
                    dst[at * 4] = s[2];
                    dst[at * 4 + 1] = s[1];
                    dst[at * 4 + 2] = s[0];
                    dst[at * 4 + 3] = 255;
                    break;
                case PPA_MOD_GRAY8:
                    dst[at] = rgb_to_y(s, false);
                    break;
            }
        }
    }
}

static void write_422(const uint8_t *work, uint32_t bw, uint32_t bh, const ppa_mod_srm_t *op) {
    uint8_t *dst = op->dst;
    bool lim = op->yuv_limited;
    for (uint32_t j = 0; j < bh; j++) {
        const uint8_t *s = work + (size_t)j * bw * 3;
        uint8_t *d = dst + ((size_t)(op->y + j) * op->dst_w + op->x) * 2;
        for (uint32_t i = 0; i + 1 < bw; i += 2, s += 6, d += 4) {
            uint8_t u, v;
            rgb_to_uv((s[0] + s[3]) >> 1, (s[1] + s[4]) >> 1, (s[2] + s[5]) >> 1, lim, &u, &v);
            uint8_t y0 = rgb_to_y(s, lim), y1 = rgb_to_y(s + 3, lim);
            if (op->dst_fmt == PPA_MOD_YUY2) {
                d[0] = y0; d[1] = u; d[2] = y1; d[3] = v;
            } else {
                d[0] = u; d[1] = y0; d[2] = v; d[3] = y1;
            }
        }
    }
}

static void write_420(const uint8_t *work, uint32_t bw, uint32_t bh, const ppa_mod_srm_t *op) {
    uint8_t *dst = op->dst;
    bool lim = op->yuv_limited;
    size_t line = (size_t)op->dst_w * 3 / 2;
    for (uint32_t j = 0; j + 1 < bh; j += 2) {
        const uint8_t *a = work + (size_t)j * bw * 3;
        const uint8_t *b = a + (size_t)bw * 3;
        uint8_t *l0 = dst + (size_t)(op->y + j) * line + (op->x >> 1) * 3;
        uint8_t *l1 = l0 + line;
        for (uint32_t i = 0; i + 1 < bw; i += 2, a += 6, b += 6, l0 += 3, l1 += 3) {
            uint8_t u, v;
            rgb_to_uv((a[0] + a[3] + b[0] + b[3]) >> 2, (a[1] + a[4] + b[1] + b[4]) >> 2,
                (a[2] + a[5] + b[2] + b[5]) >> 2, lim, &u, &v);
            l0[0] = u;
            l0[1] = rgb_to_y(a, lim);
            l0[2] = rgb_to_y(a + 3, lim);
            l1[0] = v;
            l1[1] = rgb_to_y(b, lim);
            l1[2] = rgb_to_y(b + 3, lim);
        }
    }
}

// --- the operation ----------------------------------------------------------------

const char *ppa_sw_srm(const ppa_mod_srm_t *op) {
    uint32_t sx = op->sx, sy = op->sy;
    uint32_t sw = op->sw ? op->sw : op->src_w, sh = op->sh ? op->sh : op->src_h;
    uint32_t bw = op->w ? op->w : op->dst_w, bh = op->h ? op->h : op->dst_h;
    int rot = ((op->rotate / 90) % 4 + 4) % 4;
    bool turn = rot == 1 || rot == 3;
    // the scaled picture before rotation: rotating it fills the block
    uint32_t uw = turn ? bh : bw, uh = turn ? bw : bh;
    uint8_t *work = malloc((size_t)bw * bh * 3);
    if (work == NULL) {
        return "no memory for the working picture";
    }
    for (uint32_t oy = 0; oy < bh; oy++) {
        for (uint32_t ox = 0; ox < bw; ox++) {
            // undo the mirror, then the counter-clockwise rotation
            uint32_t mx = op->mirror_x ? bw - 1 - ox : ox;
            uint32_t my = op->mirror_y ? bh - 1 - oy : oy;
            uint32_t ux, uy;
            switch (rot) {
                case 1: ux = uw - 1 - my; uy = mx; break;              // 90 CCW
                case 2: ux = uw - 1 - mx; uy = uh - 1 - my; break;     // 180
                case 3: ux = my; uy = uh - 1 - mx; break;              // 270 CCW
                default: ux = mx; uy = my; break;
            }
            uint32_t x = sx + (uint32_t)(((uint64_t)ux * sw) / uw);
            uint32_t y = sy + (uint32_t)(((uint64_t)uy * sh) / uh);
            read_px(op->src, op->src_fmt, op->src_w, x, y, op->swap, op->yuv_limited,
                work + ((size_t)oy * bw + ox) * 3);
        }
    }
    switch (op->dst_fmt) {
        case PPA_MOD_YUY2:
        case PPA_MOD_UYVY:
            write_422(work, bw, bh, op);
            break;
        case PPA_MOD_YUV420:
            write_420(work, bw, bh, op);
            break;
        default:
            write_rgb(work, bw, bh, op);
            break;
    }
    free(work);
    return NULL;
}

// Checks shared by both paths. NULL when the operation is well formed.
const char *ppa_sw_check(const ppa_mod_srm_t *op, size_t src_len) {
    if (op->src_fmt < PPA_MOD_RGB565 || op->src_fmt > PPA_MOD_YUV420
        || op->dst_fmt < PPA_MOD_RGB565 || op->dst_fmt > PPA_MOD_YUV420) {
        return "unknown pixel format";
    }
    if (op->rotate % 90) {
        return "rotate is 0, 90, 180 or 270";
    }
    uint32_t sw = op->sw ? op->sw : op->src_w, sh = op->sh ? op->sh : op->src_h;
    uint32_t bw = op->w ? op->w : op->dst_w, bh = op->h ? op->h : op->dst_h;
    if (!op->src_w || !op->src_h || !op->dst_w || !op->dst_h || !sw || !sh || !bw || !bh) {
        return "sizes must be positive";
    }
    if (op->sx + sw > op->src_w || op->sy + sh > op->src_h) {
        return "the source block is outside the source picture";
    }
    if (op->x + bw > op->dst_w || op->y + bh > op->dst_h) {
        return "the destination block is outside the destination picture";
    }
    if (src_len < ppa_mod_size(op->src_fmt, op->src_w, op->src_h)) {
        return "the source buffer is smaller than its picture";
    }
    if (op->dst_len < ppa_mod_size(op->dst_fmt, op->dst_w, op->dst_h)) {
        return "the destination buffer is smaller than its picture";
    }
    bool yuv_src = op->src_fmt >= PPA_MOD_YUY2, yuv_dst = op->dst_fmt >= PPA_MOD_YUY2;
    if ((yuv_src && ((op->src_w | op->sx | sw) & 1)) || (yuv_dst && ((op->dst_w | op->x | bw) & 1))) {
        return "YUV pictures and blocks are an even number of pixels wide";
    }
    if ((op->src_fmt == PPA_MOD_YUV420 && ((op->src_h | op->sy | sh) & 1))
        || (op->dst_fmt == PPA_MOD_YUV420 && ((op->dst_h | op->y | bh) & 1))) {
        return "YUV420 pictures and blocks are an even number of lines high";
    }
    return NULL;
}
