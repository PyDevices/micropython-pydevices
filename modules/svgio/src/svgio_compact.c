// svgio's compact form: a drawing as records of numbers, expanded here to the
// SVG text it stands for.
//
// A program that draws from code would otherwise write every coordinate out
// as text, in Python, for the parser to read straight back. In the compact
// form a shape is an opcode and its numbers as the runtime's own floats; this
// file writes the text, each number printed by the formatter Python's "%.5g"
// uses, so the result is character for character what the program would have
// written, and what NanoSVG then draws is the same picture.
//
// A drawing is five bytes, 0x01 "SVG" and the size of a float (4 or 8), then
// records to its end. Numbers are little-endian. F is a float; a paint is a
// u32: 0xFFFFFFFF for "none", under 0x1000000 a colour 0xRRGGBB, otherwise
// gradient number paint - 0x1000000, in the order the gradients come.
//
//   style = fill:paint stroke:paint width:F opacity:F n:u16 extra[n]
//           stroke 0xFFFFFFFE writes no stroke attributes; with a stroke the
//           caps and joins are round, unless extra names stroke-linecap or
//           stroke-linejoin. opacity is written when under 1. extra is
//           attributes as text, written after a space.
//    1 circle   cx cy r :F, style
//    2 ellipse  cx cy rx ry rot :F, style      rot turns it about its centre
//    3 rect     x y w h r :F, style            r, if not 0, is its rx
//    4 path     n:u32 data[n], style
//    6 group    opacity:F n:u16 transform[n]   transform as text
//    7          closes a group
//    8 group    x y rot scale :F flip:u8       translate, rotate, scale; flip
//                                              mirrors it left to right
//   10 view     x y w h :F                     the <svg> element; first
//   11 linear   x1 y1 x2 y2 :F user:u8 n:u8 stop[n]
//   12 radial   cx cy r :F focus:u8 fx fy :F user:u8 n:u8 stop[n]
//               stop = offset:F colour:u32 opacity:F
//               user: coordinates in drawing units, not fractions of the box
//   13          ends the gradients; the shapes follow
//   14          ends the drawing
//
// Path data is segments: 'M' x y, 'L' x y, 'C' six numbers, 'Q' four, 'Z',
// 'P' x y (the pair alone, no letter), and 'T' n:u16 text[n] for path data
// written as text, which takes anything SVG path data can say.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/formatfloat.h"
#include "py/mpconfig.h"
#include "py/obj.h"

#include "svgio_core.h"

#define SVGIO_NO_PAINT 0xFFFFFFFFu
#define SVGIO_NO_STROKE 0xFFFFFFFEu
#define SVGIO_GRADIENT 0x1000000u

typedef struct {
    const uint8_t *p, *end;     // what is left to read
    char *out;                  // the text so far
    size_t len, cap;
    bool bad;                   // the records ran out or made no sense
    bool full;                  // no memory for the text
} svgio_expander_t;

bool svgio_is_compact(const uint8_t *src, size_t len) {
    return len >= 5 && memcmp(src, "\x01SVG", 4) == 0;
}

// --- writing -----------------------------------------------------------------

static void svgio_put(svgio_expander_t *e, const char *s, size_t n) {
    if (e->full) {
        return;
    }
    if (e->len + n + 1 > e->cap) {
        size_t cap = e->cap + e->cap / 2 + n + 256;
        char *grown = svgio_mem_alloc(cap);
        if (grown == NULL) {
            e->full = true;
            return;
        }
        memcpy(grown, e->out, e->len);
        svgio_mem_free(e->out);
        e->out = grown;
        e->cap = cap;
    }
    memcpy(e->out + e->len, s, n);
    e->len += n;
}

static void svgio_puts(svgio_expander_t *e, const char *s) {
    svgio_put(e, s, strlen(s));
}

// A number as Python's "%.5g" % v writes it: the same formatter, so the same
// characters.
static void svgio_put_num(svgio_expander_t *e, mp_float_t v) {
    char buf[32];
    int n = mp_format_float(v, buf, sizeof(buf), 'g', 5, '\0');
    svgio_put(e, buf, n);
}

static void svgio_put_uint(svgio_expander_t *e, unsigned v) {
    char buf[12];
    int n = sizeof(buf);
    do {
        buf[--n] = '0' + v % 10;
        v /= 10;
    } while (v != 0);
    svgio_put(e, buf + n, sizeof(buf) - n);
}

static void svgio_put_paint(svgio_expander_t *e, uint32_t paint) {
    if (paint == SVGIO_NO_PAINT) {
        svgio_puts(e, "none");
    } else if (paint >= SVGIO_GRADIENT) {
        svgio_puts(e, "url(#g");
        svgio_put_uint(e, paint - SVGIO_GRADIENT);
        svgio_puts(e, ")");
    } else {
        char buf[7] = "#";
        for (int i = 0; i < 6; i++) {
            buf[1 + i] = "0123456789abcdef"[(paint >> (20 - 4 * i)) & 15];
        }
        svgio_put(e, buf, 7);
    }
}

// --- reading -----------------------------------------------------------------

static const uint8_t *svgio_take(svgio_expander_t *e, size_t n) {
    static const uint8_t zeros[8];
    if ((size_t)(e->end - e->p) < n) {
        e->bad = true;
        e->p = e->end;
        return n <= sizeof(zeros) ? zeros : NULL;
    }
    const uint8_t *at = e->p;
    e->p += n;
    return at;
}

static unsigned svgio_u8(svgio_expander_t *e) {
    return svgio_take(e, 1)[0];
}

static unsigned svgio_u16(svgio_expander_t *e) {
    const uint8_t *b = svgio_take(e, 2);
    return b[0] | b[1] << 8;
}

static uint32_t svgio_u32(svgio_expander_t *e) {
    const uint8_t *b = svgio_take(e, 4);
    return b[0] | b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static mp_float_t svgio_num(svgio_expander_t *e) {
    mp_float_t v;
    memcpy(&v, svgio_take(e, sizeof(v)), sizeof(v));
    return v;
}

// n bytes of text, copied out as they are.
static void svgio_copy(svgio_expander_t *e, size_t n) {
    const uint8_t *text = svgio_take(e, n);
    if (!e->bad && n != 0) {
        svgio_put(e, (const char *)text, n);
    }
}

// name="number"
static void svgio_attr(svgio_expander_t *e, const char *name, mp_float_t v) {
    svgio_puts(e, name);
    svgio_put_num(e, v);
    svgio_puts(e, "\"");
}

// True when the n bytes at s hold word.
static bool svgio_holds(const uint8_t *s, size_t n, const char *word) {
    size_t w = strlen(word);
    for (size_t i = 0; i + w <= n; i++) {
        if (memcmp(s + i, word, w) == 0) {
            return true;
        }
    }
    return false;
}

// --- records -----------------------------------------------------------------

// A shape's style, and its extra attributes: before them goes `first` (a
// rect's rx), after them `last` (an ellipse's turn), as the text form has it.
static void svgio_style(svgio_expander_t *e, const char *first, mp_float_t first_v,
    bool turn, mp_float_t rot, mp_float_t cx, mp_float_t cy) {
    uint32_t fill = svgio_u32(e), stroke = svgio_u32(e);
    mp_float_t width = svgio_num(e), opacity = svgio_num(e);
    size_t n = svgio_u16(e);
    svgio_puts(e, " fill=\"");
    svgio_put_paint(e, fill);
    svgio_puts(e, "\"");
    if (stroke != SVGIO_NO_STROKE) {
        svgio_puts(e, " stroke=\"");
        svgio_put_paint(e, stroke);
        svgio_attr(e, "\" stroke-width=\"", width);
        // An attribute given twice is an error to a strict XML reader.
        const size_t have = (size_t)(e->end - e->p) < n ? 0 : n;
        if (!svgio_holds(e->p, have, "stroke-linecap")) {
            svgio_puts(e, " stroke-linecap=\"round\"");
        }
        if (!svgio_holds(e->p, have, "stroke-linejoin")) {
            svgio_puts(e, " stroke-linejoin=\"round\"");
        }
    }
    if (opacity < 1) {
        svgio_attr(e, " opacity=\"", opacity);
    }
    if (first != NULL || n != 0 || turn) {
        svgio_puts(e, " ");
    }
    if (first != NULL) {
        svgio_attr(e, first, first_v);
        svgio_puts(e, " ");
    }
    svgio_copy(e, n);
    if (turn) {
        if (n != 0) {
            svgio_puts(e, " ");
        }
        svgio_puts(e, "transform=\"rotate(");
        svgio_put_num(e, rot);
        svgio_puts(e, " ");
        svgio_put_num(e, cx);
        svgio_puts(e, " ");
        svgio_put_num(e, cy);
        svgio_puts(e, ")\"");
    }
    svgio_puts(e, "/>");
}

static void svgio_pair(svgio_expander_t *e) {
    svgio_put_num(e, svgio_num(e));
    svgio_puts(e, " ");
    svgio_put_num(e, svgio_num(e));
}

static void svgio_path_data(svgio_expander_t *e, size_t n) {
    if ((size_t)(e->end - e->p) < n) {
        e->bad = true;
        return;
    }
    const uint8_t *all = e->end;
    e->end = e->p + n;
    while (e->p < e->end && !e->bad) {
        unsigned op = svgio_u8(e);
        switch (op) {
            case 'M':
            case 'L':
                svgio_put(e, (const char *)e->p - 1, 1);
                svgio_pair(e);
                break;
            case 'P':
                svgio_pair(e);
                break;
            case 'C':
            case 'Q':
                svgio_put(e, (const char *)e->p - 1, 1);
                for (int i = op == 'C' ? 3 : 2; i > 0; i--) {
                    svgio_pair(e);
                    if (i > 1) {
                        svgio_puts(e, " ");
                    }
                }
                break;
            case 'Z':
                svgio_puts(e, "Z");
                break;
            case 'T':
                svgio_copy(e, svgio_u16(e));
                break;
            default:
                e->bad = true;
        }
    }
    e->end = all;
}

static void svgio_stops(svgio_expander_t *e, unsigned n) {
    while (n-- > 0 && !e->bad) {
        mp_float_t offset = svgio_num(e);
        uint32_t color = svgio_u32(e);
        mp_float_t opacity = svgio_num(e);
        svgio_attr(e, "<stop offset=\"", offset);
        svgio_puts(e, " stop-color=\"");
        svgio_put_paint(e, color);
        svgio_puts(e, "\"");
        if (opacity < 1) {
            svgio_attr(e, " stop-opacity=\"", opacity);
        }
        svgio_puts(e, "/>");
    }
}

char *svgio_expand(const uint8_t *src, size_t len, size_t *out_len, const char **why) {
    *why = NULL;
    if (!svgio_is_compact(src, len) || src[4] != sizeof(mp_float_t)) {
        *why = "a compact drawing made for another size of float";
        return NULL;
    }
    svgio_expander_t ex = { src + 5, src + len, NULL, 0, 0, false, false };
    svgio_expander_t *e = &ex;
    // Text runs to three or four times the records; it grows if it must.
    e->cap = len * 4 + 256;
    e->out = svgio_mem_alloc(e->cap);
    e->full = e->out == NULL;
    unsigned gradients = 0;
    while (e->p < e->end && !e->bad && !e->full) {
        switch (svgio_u8(e)) {
            case 1: {
                svgio_attr(e, "<circle cx=\"", svgio_num(e));
                svgio_attr(e, " cy=\"", svgio_num(e));
                svgio_attr(e, " r=\"", svgio_num(e));
                svgio_style(e, NULL, 0, false, 0, 0, 0);
                break;
            }
            case 2: {
                mp_float_t cx = svgio_num(e), cy = svgio_num(e);
                svgio_attr(e, "<ellipse cx=\"", cx);
                svgio_attr(e, " cy=\"", cy);
                svgio_attr(e, " rx=\"", svgio_num(e));
                svgio_attr(e, " ry=\"", svgio_num(e));
                mp_float_t rot = svgio_num(e);
                svgio_style(e, NULL, 0, rot != 0, rot, cx, cy);
                break;
            }
            case 3: {
                svgio_attr(e, "<rect x=\"", svgio_num(e));
                svgio_attr(e, " y=\"", svgio_num(e));
                svgio_attr(e, " width=\"", svgio_num(e));
                svgio_attr(e, " height=\"", svgio_num(e));
                mp_float_t r = svgio_num(e);
                svgio_style(e, r != 0 ? "rx=\"" : NULL, r, false, 0, 0, 0);
                break;
            }
            case 4:
                svgio_puts(e, "<path d=\"");
                svgio_path_data(e, svgio_u32(e));
                svgio_puts(e, "\"");
                svgio_style(e, NULL, 0, false, 0, 0, 0);
                break;
            case 6: {
                mp_float_t opacity = svgio_num(e);
                size_t n = svgio_u16(e);
                svgio_puts(e, "<g");
                if (n != 0) {
                    svgio_puts(e, " transform=\"");
                    svgio_copy(e, n);
                    svgio_puts(e, "\"");
                }
                if (opacity < 1) {
                    svgio_attr(e, " opacity=\"", opacity);
                }
                svgio_puts(e, ">");
                break;
            }
            case 7:
                svgio_puts(e, "</g>");
                break;
            case 8: {
                svgio_puts(e, "<g transform=\"translate(");
                svgio_pair(e);
                svgio_puts(e, ")");
                mp_float_t rot = svgio_num(e), scale = svgio_num(e);
                if (rot != 0) {
                    svgio_puts(e, " rotate(");
                    svgio_put_num(e, rot);
                    svgio_puts(e, ")");
                }
                if (svgio_u8(e)) {
                    svgio_puts(e, " scale(");
                    svgio_put_num(e, -scale);
                    svgio_puts(e, " ");
                    svgio_put_num(e, scale);
                    svgio_puts(e, ")");
                } else if (scale != 1) {
                    svgio_puts(e, " scale(");
                    svgio_put_num(e, scale);
                    svgio_puts(e, ")");
                }
                svgio_puts(e, "\">");
                break;
            }
            case 10: {
                mp_float_t x = svgio_num(e), y = svgio_num(e), w = svgio_num(e), h = svgio_num(e);
                svgio_attr(e, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"", w);
                svgio_attr(e, " height=\"", h);
                svgio_puts(e, " viewBox=\"");
                svgio_put_num(e, x);
                svgio_puts(e, " ");
                svgio_put_num(e, y);
                svgio_puts(e, " ");
                svgio_put_num(e, w);
                svgio_puts(e, " ");
                svgio_put_num(e, h);
                svgio_puts(e, "\"><defs>");
                break;
            }
            case 11: {
                svgio_puts(e, "<linearGradient id=\"g");
                svgio_put_uint(e, gradients++);
                svgio_attr(e, "\" x1=\"", svgio_num(e));
                svgio_attr(e, " y1=\"", svgio_num(e));
                svgio_attr(e, " x2=\"", svgio_num(e));
                svgio_attr(e, " y2=\"", svgio_num(e));
                if (svgio_u8(e)) {
                    svgio_puts(e, " gradientUnits=\"userSpaceOnUse\"");
                }
                svgio_puts(e, ">");
                svgio_stops(e, svgio_u8(e));
                svgio_puts(e, "</linearGradient>");
                break;
            }
            case 12: {
                svgio_puts(e, "<radialGradient id=\"g");
                svgio_put_uint(e, gradients++);
                svgio_attr(e, "\" cx=\"", svgio_num(e));
                svgio_attr(e, " cy=\"", svgio_num(e));
                svgio_attr(e, " r=\"", svgio_num(e));
                unsigned focus = svgio_u8(e);
                mp_float_t fx = svgio_num(e), fy = svgio_num(e);
                if (focus) {
                    svgio_attr(e, " fx=\"", fx);
                    svgio_attr(e, " fy=\"", fy);
                }
                if (svgio_u8(e)) {
                    svgio_puts(e, " gradientUnits=\"userSpaceOnUse\"");
                }
                svgio_puts(e, ">");
                svgio_stops(e, svgio_u8(e));
                svgio_puts(e, "</radialGradient>");
                break;
            }
            case 13:
                svgio_puts(e, "</defs>");
                break;
            case 14:
                svgio_puts(e, "</svg>");
                break;
            default:
                e->bad = true;
        }
    }
    if (e->bad || e->full) {
        svgio_mem_free(e->out);
        *why = e->full ? "not enough memory to parse the SVG" : "not a drawing in svgio's compact form";
        return NULL;
    }
    e->out[e->len] = '\0';      // svgio_put keeps room for it
    *out_len = e->len;
    return e->out;
}
