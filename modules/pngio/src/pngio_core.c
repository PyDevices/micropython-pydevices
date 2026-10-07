// pngio's engine. See pngio_core.h.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "pngio_core.h"

// --- checksums -------------------------------------------------------------

static uint32_t crc_table[256];

static void crc_init(void) {
    if (crc_table[1] != 0) {
        return;
    }
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;     // keeps b from overflowing
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// --- formats ---------------------------------------------------------------

int pngio_in_bpp(int fmt) {
    return fmt == PNGIO_RGB565 ? 2 : fmt == PNGIO_GS8 ? 1 : 3;
}

int pngio_channels(int fmt) {
    return fmt == PNGIO_GS8 ? 1 : 3;
}

// --- deflate: fixed Huffman, hashed LZ77 -------------------------------------

#define WINDOW (32768u)
#define HASH_BITS (13)          // 32 kB of heads: on a board it is PSRAM, and smaller misses less
#define MAX_INSERT (4)          // a longer match's positions aren't hashed (zlib's fast level)
#define MIN_MATCH (3)
#define MAX_MATCH (258)

typedef struct {
    uint8_t *out, *end;
    uint32_t bits;
    int nbits;
    bool overflow;
} bitw_t;

static void bw_put(bitw_t *w, uint32_t v, int n) {
    w->bits |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (w->out < w->end) {
            *w->out++ = (uint8_t)w->bits;
        } else {
            w->overflow = true;
        }
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

// Huffman codes go out most significant bit first, so they are reversed into
// the LSB-first stream.
static uint32_t rev(uint32_t v, int n) {
    uint32_t r = 0;
    while (n--) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

static void put_litlen(bitw_t *w, int sym) {
    if (sym < 144) {
        bw_put(w, rev(0x30 + sym, 8), 8);
    } else if (sym < 256) {
        bw_put(w, rev(0x190 + sym - 144, 9), 9);
    } else if (sym < 280) {
        bw_put(w, rev(sym - 256, 7), 7);
    } else {
        bw_put(w, rev(0xC0 + sym - 280, 8), 8);
    }
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
                                        9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(bitw_t *w, uint32_t len, uint32_t dist) {
    int i = 28;
    while (len_base[i] > len) {
        i--;
    }
    put_litlen(w, 257 + i);
    if (len_extra[i]) {
        bw_put(w, len - len_base[i], len_extra[i]);
    }
    int d = 29;
    while (dist_base[d] > dist) {
        d--;
    }
    bw_put(w, rev(d, 5), 5);
    if (dist_extra[d]) {
        bw_put(w, dist - dist_base[d], dist_extra[d]);
    }
}

static uint32_t hash3(const uint8_t *p) {
    return ((((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]) * 2654435761u) >> (32 - HASH_BITS);
}

// The chain depth for a level; level 1 looks at the newest candidate only.
static int chain_depth(int level) {
    return level <= 1 ? 1 : level >= 9 ? 128 : 1 << (level - 1);
}

static void deflate_fixed(bitw_t *w, const uint8_t *src, size_t n, int level, uint32_t *head, uint32_t *prev) {
    bw_put(w, 1, 1);    // BFINAL
    bw_put(w, 1, 2);    // BTYPE 01: fixed Huffman
    const int depth = chain_depth(level);
    // head[] and prev[] hold position + 1, so 0 is empty
    memset(head, 0, sizeof(uint32_t) << HASH_BITS);
    size_t i = 0;
    while (i < n) {
        uint32_t best_len = 0, best_dist = 0;
        if (i + MIN_MATCH <= n) {
            const uint32_t h = hash3(src + i);
            uint32_t cand = head[h];
            int tries = depth;
            const uint32_t maxlen = (uint32_t)(n - i < MAX_MATCH ? n - i : MAX_MATCH);
            while (cand && tries--) {
                const size_t c = cand - 1;
                if (i - c > WINDOW) {
                    break;
                }
                if (src[c + best_len] == src[i + best_len] && src[c] == src[i]) {
                    uint32_t l = 0;
                    while (l < maxlen && src[c + l] == src[i + l]) {
                        l++;
                    }
                    if (l > best_len) {
                        best_len = l;
                        best_dist = (uint32_t)(i - c);
                        if (l == maxlen) {
                            break;
                        }
                    }
                }
                // the chain is a ring: an entry older than the window can have
                // been overwritten by a newer position, so it must lead back
                const uint32_t next = prev ? prev[c & (WINDOW - 1)] : 0;
                cand = (next && next - 1 < c) ? next : 0;
            }
            if (prev) {
                prev[i & (WINDOW - 1)] = head[h];
            }
            head[h] = (uint32_t)i + 1;
        }
        if (best_len >= MIN_MATCH) {
            put_match(w, best_len, best_dist);
            const size_t stop = i + best_len;
            if (best_len <= MAX_INSERT) {
                // a short match: hash the positions it covers, so later matches
                // can start there. A long one (a flat screen's runs) is skipped
                // whole: hashing every byte of it cost most of a board's encode.
                for (i++; i < stop; i++) {
                    if (i + MIN_MATCH <= n) {
                        const uint32_t h = hash3(src + i);
                        if (prev) {
                            prev[i & (WINDOW - 1)] = head[h];
                        }
                        head[h] = (uint32_t)i + 1;
                    }
                }
            }
            i = stop;
        } else {
            put_litlen(w, src[i]);
            i++;
        }
    }
    put_litlen(w, 256);     // end of block
    if (w->nbits) {
        bw_put(w, 0, 8 - w->nbits);
    }
}

// --- PNG encode --------------------------------------------------------------

size_t pngio_encode_bound(uint32_t w, uint32_t h, int fmt) {
    const size_t raw = (size_t)h * (1 + (size_t)w * pngio_channels(fmt));
    // fixed Huffman is at most 9 bits a byte; signature, chunks and zlib framing besides
    return raw + raw / 8 + 128;
}

size_t pngio_encode_work(uint32_t w, uint32_t h, int fmt, int level) {
    const size_t row = (size_t)w * pngio_channels(fmt);
    size_t n = (size_t)h * (1 + row)        // the filtered scanlines
        + 2 * row                           // this row and the last, unfiltered
        + (sizeof(uint32_t) << HASH_BITS);  // hash heads
    if (level > 1) {
        n += sizeof(uint32_t) * WINDOW;     // the chain
    }
    return n + 16;
}

// One input row to raw PNG bytes.
static void row_to_png(const uint8_t *s, uint8_t *d, uint32_t w, int fmt, bool swap) {
    if (fmt == PNGIO_RGB565) {
        for (uint32_t x = 0; x < w; x++, s += 2, d += 3) {
            const unsigned v = swap ? ((unsigned)s[0] << 8 | s[1]) : ((unsigned)s[1] << 8 | s[0]);
            const unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            d[0] = (uint8_t)(r << 3 | r >> 2);
            d[1] = (uint8_t)(g << 2 | g >> 4);
            d[2] = (uint8_t)(b << 3 | b >> 2);
        }
    } else {
        memcpy(d, s, (size_t)w * pngio_channels(fmt));
    }
}

static int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

// Filter `cur` (with `prev` above it) into out[1..]; out[0] is the type. Picks
// the type whose bytes, as signed, sum smallest.
static void filter_row(const uint8_t *cur, const uint8_t *prev, uint8_t *out, size_t n, int bpp) {
    uint32_t best = UINT32_MAX;
    int best_t = 0;
    for (int t = 0; t <= 4; t++) {
        if (t == 3) {
            continue;   // Average rarely wins on screens; skipping it saves a pass
        }
        uint32_t sum = 0;
        for (size_t i = 0; i < n && sum < best; i++) {
            const int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
            const int pred = t == 0 ? 0 : t == 1 ? a : t == 2 ? b : paeth(a, b, c);
            const int8_t v = (int8_t)(uint8_t)(cur[i] - pred);
            sum += (uint32_t)(v < 0 ? -v : v);
        }
        if (sum < best) {
            best = sum;
            best_t = t;
        }
    }
    out[0] = (uint8_t)best_t;
    for (size_t i = 0; i < n; i++) {
        const int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
        const int pred = best_t == 0 ? 0 : best_t == 1 ? a : best_t == 2 ? b : paeth(a, b, c);
        out[1 + i] = (uint8_t)(cur[i] - pred);
    }
}

static uint8_t *chunk(uint8_t *p, const char *type, const uint8_t *data, size_t len) {
    put_be32(p, (uint32_t)len);
    memcpy(p + 4, type, 4);
    if (data != NULL && data != p + 8) {
        memmove(p + 8, data, len);
    }
    put_be32(p + 8 + len, crc32_update(0, p + 4, len + 4));
    return p + 12 + len;
}

const char *pngio_encode(const uint8_t *src, uint32_t w, uint32_t h, uint32_t stride, int fmt,
    bool swap, int level, void *work, uint8_t *out, size_t cap, size_t *out_len) {
    if (w == 0 || h == 0 || w > 0x7FFFFFFF / 4 || h > 0x7FFFFFFF / 4) {
        return "width and height must be 1 or more";
    }
    if (fmt != PNGIO_RGB565 && fmt != PNGIO_GS8 && fmt != PNGIO_RGB888) {
        return "format must be RGB565, GS8 or RGB888";
    }
    if (cap < pngio_encode_bound(w, h, fmt)) {
        return "output buffer too small";
    }
    crc_init();
    if (stride == 0) {
        stride = w;
    }
    const int ch = pngio_channels(fmt), ibpp = pngio_in_bpp(fmt);
    const size_t row = (size_t)w * ch, line = 1 + row;
    // carve the scratch: scanlines, two raw rows, hash heads, chain
    uint8_t *scan = (uint8_t *)work;
    uint8_t *cur = scan + (size_t)h * line, *prev = cur + row;
    uintptr_t hp = (uintptr_t)(prev + row);
    hp = (hp + 3) & ~(uintptr_t)3;
    uint32_t *head = (uint32_t *)hp;
    uint32_t *chain = level > 1 ? head + (1u << HASH_BITS) : NULL;

    memset(prev, 0, row);
    for (uint32_t y = 0; y < h; y++) {
        row_to_png(src + (size_t)y * stride * ibpp, cur, w, fmt, swap);
        filter_row(cur, prev, scan + (size_t)y * line, row, ch);
        uint8_t *t = prev;
        prev = cur;
        cur = t;
    }

    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    uint8_t *p = out;
    memcpy(p, sig, 8);
    p += 8;
    uint8_t ihdr[13];
    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8] = 8;                            // bit depth
    ihdr[9] = fmt == PNGIO_GS8 ? 0 : 2;     // grey or RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;     // deflate, adaptive filters, no interlace
    p = chunk(p, "IHDR", ihdr, 13);

    // IDAT: the zlib stream is written in place after the chunk's length and type
    uint8_t *z = p + 8;
    z[0] = 0x78;    // deflate, 32 KB window
    z[1] = 0x01;    // fastest; 0x7801 is a multiple of 31
    bitw_t bw = { .out = z + 2, .end = out + cap - 4 - 4 - 12, .bits = 0, .nbits = 0, .overflow = false };
    deflate_fixed(&bw, scan, (size_t)h * line, level, head, chain);
    if (bw.overflow) {
        return "output buffer too small";
    }
    put_be32(bw.out, adler32(scan, (size_t)h * line));
    const size_t zlen = (size_t)(bw.out + 4 - z);
    p = chunk(p, "IDAT", z, zlen);
    p = chunk(p, "IEND", NULL, 0);
    *out_len = (size_t)(p - out);
    return NULL;
}

// --- PNG decode --------------------------------------------------------------

const char *pngio_parse(const uint8_t *png, size_t len, pngio_info_t *info) {
    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };
    memset(info, 0, sizeof(*info));
    if (len < 8 + 25 || memcmp(png, sig, 8) != 0) {
        return "not a PNG";
    }
    if (get_be32(png + 8) != 13 || memcmp(png + 12, "IHDR", 4) != 0) {
        return "PNG has no IHDR first";
    }
    const uint8_t *h = png + 16;
    info->width = get_be32(h);
    info->height = get_be32(h + 4);
    info->depth = h[8];
    info->ctype = h[9];
    info->interlace = h[12];
    if (info->width == 0 || info->height == 0 || info->width > 0x7FFFFFF || info->height > 0x7FFFFFF) {
        return "PNG has a bad size";
    }
    if (h[10] != 0 || h[11] != 0) {
        return "PNG uses an unknown compression or filter method";
    }
    if (info->interlace) {
        return "interlaced PNG isn't supported";
    }
    int channels;
    switch (info->ctype) {
        case 0: channels = 1; break;    // grey
        case 2: channels = 3; break;    // RGB
        case 3: channels = 1; break;    // palette
        case 4: channels = 2; break;    // grey + alpha
        case 6: channels = 4; break;    // RGBA
        default: return "PNG has an unknown colour type";
    }
    const bool low = (info->ctype == 0 || info->ctype == 3) &&
        (info->depth == 1 || info->depth == 2 || info->depth == 4);
    if (!(info->depth == 8 || low)) {
        return "PNG bit depth isn't supported (8, or 1/2/4 for grey and palette)";
    }
    info->bpp = info->depth == 8 ? channels : 1;
    info->rowbytes = ((size_t)info->width * channels * info->depth + 7) / 8;
    // PLTE, if any, before the first IDAT
    size_t pos = 8;
    while (pos + 12 <= len) {
        const uint32_t clen = get_be32(png + pos);
        const uint8_t *type = png + pos + 4;
        if (pos + 12 + clen > len) {
            return "PNG is truncated";
        }
        if (memcmp(type, "PLTE", 4) == 0) {
            if (clen % 3 || clen > 768) {
                return "PNG has a bad palette";
            }
            info->npalette = (uint16_t)(clen / 3);
            memcpy(info->palette, png + pos + 8, clen);
        } else if (memcmp(type, "IDAT", 4) == 0 || memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }
    if (info->ctype == 3 && info->npalette == 0) {
        return "palette PNG has no PLTE";
    }
    return NULL;
}

bool pngio_next_idat(const uint8_t *png, size_t len, size_t *pos, const uint8_t **data, size_t *dlen) {
    size_t p = *pos ? *pos : 8;
    while (p + 12 <= len) {
        const uint32_t clen = get_be32(png + p);
        if (p + 12 + clen > len) {
            return false;
        }
        const uint8_t *type = png + p + 4;
        const size_t next = p + 12 + clen;
        if (memcmp(type, "IDAT", 4) == 0) {
            *data = png + p + 8;
            *dlen = clen;
            *pos = next;
            return true;
        }
        if (memcmp(type, "IEND", 4) == 0) {
            return false;
        }
        p = next;
    }
    return false;
}

const char *pngio_unfilter(uint8_t ftype, uint8_t *row, const uint8_t *prev, size_t n, int bpp) {
    switch (ftype) {
        case 0:
            break;
        case 1:
            for (size_t i = bpp; i < n; i++) {
                row[i] = (uint8_t)(row[i] + row[i - bpp]);
            }
            break;
        case 2:
            for (size_t i = 0; i < n; i++) {
                row[i] = (uint8_t)(row[i] + prev[i]);
            }
            break;
        case 3:
            for (size_t i = 0; i < n; i++) {
                const int a = i >= (size_t)bpp ? row[i - bpp] : 0;
                row[i] = (uint8_t)(row[i] + ((a + prev[i]) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < n; i++) {
                const int a = i >= (size_t)bpp ? row[i - bpp] : 0, c = i >= (size_t)bpp ? prev[i - bpp] : 0;
                row[i] = (uint8_t)(row[i] + paeth(a, prev[i], c));
            }
            break;
        default:
            return "PNG has a bad filter type";
    }
    return NULL;
}

static uint16_t to565(unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void pngio_row_to_565(const pngio_info_t *info, const uint8_t *row, uint16_t *dst, uint32_t w) {
    const int d = info->depth;
    for (uint32_t x = 0; x < w; x++) {
        unsigned r, g, b;
        switch (info->ctype) {
            case 2:
                r = row[3 * x], g = row[3 * x + 1], b = row[3 * x + 2];
                break;
            case 6:
                r = row[4 * x], g = row[4 * x + 1], b = row[4 * x + 2];
                break;
            case 4:
                r = g = b = row[2 * x];
                break;
            default: {      // grey or palette, at 1, 2, 4 or 8 bits
                unsigned v;
                if (d == 8) {
                    v = row[x];
                } else {
                    const unsigned per = 8 / d, shift = 8 - d - (x % per) * d;
                    v = (row[x / per] >> shift) & ((1u << d) - 1);
                }
                if (info->ctype == 3) {
                    if (v >= info->npalette) {
                        v = 0;
                    }
                    r = info->palette[3 * v], g = info->palette[3 * v + 1], b = info->palette[3 * v + 2];
                } else {
                    // widen the grey to 8 bits
                    v = d == 8 ? v : d == 4 ? v * 17 : d == 2 ? v * 85 : v * 255;
                    r = g = b = v;
                }
                break;
            }
        }
        dst[x] = to565(r, g, b);
    }
}
