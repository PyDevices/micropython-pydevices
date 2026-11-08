// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// A HID report descriptor read into the input fields it declares, so the
// reports of any keyboard, mouse, gamepad or media remote can be taken
// apart by what each field means rather than where it sits. Plain C with
// no MicroPython in it: mod_btclassic_hid.c includes it, and so can a test
// on a desktop.

#ifndef BTC_HID_PARSE_H
#define BTC_HID_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define HIDP_MAX_FIELDS 48
#define HIDP_MAX_USAGES 16
#define HIDP_MAX_IDS 16

// What an application collection is, from its usage.
enum {
    HIDP_OTHER = 0,
    HIDP_KEYBOARD,
    HIDP_MOUSE,
    HIDP_GAMEPAD,       // a gamepad or a joystick
    HIDP_CONSUMER,
};

typedef struct {
    uint8_t id;         // report ID, 0 without IDs
    uint8_t kind;       // HIDP_*, from the application collection
    uint8_t array;      // an array: each element holds a usage index
    uint8_t size;       // bits per element
    uint8_t count;      // elements
    uint16_t page;
    uint16_t usage;     // the first element's usage (arrays: usage minimum)
    uint16_t bit;       // where the first element starts, after the ID byte
    int32_t lmin, lmax;
} hidp_field_t;

typedef struct {
    hidp_field_t f[HIDP_MAX_FIELDS];
    uint8_t n;
    bool ids;
} hidp_t;

static uint32_t hidp_unsigned(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        v |= (uint32_t)p[i] << (8 * i);
    }
    return v;
}

static int32_t hidp_signed(const uint8_t *p, int n) {
    uint32_t v = hidp_unsigned(p, n);
    if (n == 1) {
        return (int8_t)v;
    }
    if (n == 2) {
        return (int16_t)v;
    }
    return (int32_t)v;
}

static void hidp_add(hidp_t *h, const hidp_field_t *f) {
    if (h->n < HIDP_MAX_FIELDS) {
        h->f[h->n++] = *f;
    }
}

// Read the descriptor's input items. Feature and output items only move
// nothing, since they live in reports of their own.
static void hidp_parse(hidp_t *h, const uint8_t *d, size_t len) {
    memset(h, 0, sizeof(*h));
    struct {
        uint16_t page;
        int32_t lmin, lmax;
        uint32_t size, count;
        uint8_t id;
    } g = {0}, stack[4];
    int depth = 0;
    uint32_t usages[HIDP_MAX_USAGES];
    int nusages = 0;
    uint32_t umin = 0, umax = 0;
    bool range = false;
    uint8_t kind = HIDP_OTHER;
    int collections = 0;
    struct {
        uint8_t id;
        uint16_t bits;
    } offs[HIDP_MAX_IDS];
    int noffs = 0;

    size_t i = 0;
    while (i < len) {
        uint8_t b = d[i];
        if (b == 0xfe) {                    // a long item: skip it
            if (i + 2 >= len) {
                break;
            }
            i += 3 + d[i + 1];
            continue;
        }
        int n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + n > len) {
            break;
        }
        const uint8_t *p = d + i + 1;
        uint32_t u = hidp_unsigned(p, n);
        i += 1 + n;
        uint8_t type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                    // global
            switch (tag) {
                case 0: g.page = (uint16_t)u; break;
                case 1: g.lmin = hidp_signed(p, n); break;
                case 2: g.lmax = hidp_signed(p, n); break;
                case 7: g.size = u; break;
                case 8: g.id = (uint8_t)u; h->ids = true; break;
                case 9: g.count = u; break;
                case 10: if (depth < 4) { stack[depth++] = g; } break;
                case 11: if (depth > 0) { g = stack[--depth]; } break;
            }
            continue;
        }
        if (type == 2) {                    // local
            // A 4-byte usage carries its own page in the top half.
            uint32_t full = n == 4 ? u : ((uint32_t)g.page << 16) | u;
            switch (tag) {
                case 0: if (nusages < HIDP_MAX_USAGES) { usages[nusages++] = full; } break;
                case 1: umin = full; range = true; break;
                case 2: umax = full; range = true; break;
            }
            continue;
        }
        if (type != 0) {
            continue;
        }
        // main
        if (tag == 10) {                    // collection
            if (u == 1 && collections == 0) {   // the outermost application
                uint32_t usage = nusages ? usages[0] : 0;
                uint16_t page = usage >> 16, id = usage & 0xffff;
                kind = HIDP_OTHER;
                if (page == 0x01 && id == 0x06) {
                    kind = HIDP_KEYBOARD;
                } else if (page == 0x01 && (id == 0x02 || id == 0x01)) {
                    kind = HIDP_MOUSE;
                } else if (page == 0x01 && (id == 0x04 || id == 0x05)) {
                    kind = HIDP_GAMEPAD;
                } else if (page == 0x0c) {
                    kind = HIDP_CONSUMER;
                }
            }
            collections++;
        } else if (tag == 12) {             // end collection
            if (collections > 0) {
                collections--;
            }
        } else if (tag == 8) {              // input
            int k;
            for (k = 0; k < noffs && offs[k].id != g.id; k++) {
            }
            if (k == noffs) {
                if (noffs == HIDP_MAX_IDS) {
                    goto clear;
                }
                offs[noffs].id = g.id;
                offs[noffs].bits = 0;
                noffs++;
            }
            uint16_t at = offs[k].bits;
            offs[k].bits += (uint16_t)(g.size * g.count);
            bool constant = u & 1, array = !(u & 2);
            if (constant || g.size == 0 || g.size > 32 || g.count == 0) {
                goto clear;
            }
            hidp_field_t f = {
                .id = g.id, .kind = kind, .array = array, .size = (uint8_t)g.size,
                .bit = at, .lmin = g.lmin, .lmax = g.lmax,
            };
            if (array || (range && nusages == 0)) {
                // One entry: an array of indexes, or a run of usages.
                uint32_t first = range ? umin : (nusages ? usages[0] : 0);
                f.page = first >> 16 ? first >> 16 : g.page;
                f.usage = first & 0xffff;
                f.count = (uint8_t)(g.count > 255 ? 255 : g.count);
                hidp_add(h, &f);
            } else {
                // An entry per element, each with its own usage; the last
                // usage stands for any element past the list.
                for (uint32_t e = 0; e < g.count; e++) {
                    uint32_t usage = nusages ? usages[e < (uint32_t)nusages ? e : (uint32_t)nusages - 1] : 0;
                    f.page = usage >> 16 ? usage >> 16 : g.page;
                    f.usage = usage & 0xffff;
                    f.count = 1;
                    f.bit = (uint16_t)(at + e * g.size);
                    hidp_add(h, &f);
                }
            }
        }
    clear:
        if (type == 0) {
            nusages = 0;
            range = false;
            umin = umax = 0;
        }
    }
}

// One element's value: signed when the field's logical minimum is below 0.
static int32_t hidp_get(const hidp_field_t *f, const uint8_t *r, size_t len, int e) {
    uint32_t bit = f->bit + (uint32_t)e * f->size;
    uint32_t v = 0;
    for (uint32_t k = 0; k < f->size; k++) {
        uint32_t at = bit + k, byte = at >> 3;
        if (byte >= len) {
            return 0;
        }
        v |= (uint32_t)((r[byte] >> (at & 7)) & 1) << k;
    }
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1)) & 1) {
        v |= ~0u << f->size;
    }
    return (int32_t)v;
}

// An axis on -127..127 whatever its range, centred when the range is.
static int hidp_axis(const hidp_field_t *f, int32_t v) {
    int64_t lo = f->lmin, hi = f->lmax;
    if (hi <= lo) {
        // No usable range: take it as the field's own width.
        if (f->lmin < 0) {
            lo = -(1LL << (f->size - 1));
            hi = (1LL << (f->size - 1)) - 1;
        } else {
            lo = 0;
            hi = (1LL << f->size) - 1;
        }
    }
    int64_t s = ((int64_t)v - lo) * 254 / (hi - lo) - 127;
    return s < -127 ? -127 : s > 127 ? 127 : (int)s;
}

typedef struct {
    uint8_t kind;           // HIDP_*, or HIDP_OTHER for nothing found
    uint8_t modifiers;      // keyboard
    uint8_t nkeys;
    uint8_t keys[6];
    uint32_t buttons;       // mouse, gamepad
    int32_t x, y, wheel;    // mouse: as sent
    int hat;                // gamepad: 0..7 clockwise from up, -1 centred
    int ax, ay, az, arz;    // gamepad: -127..127
    uint16_t usage;         // consumer
} hidp_report_t;

// Take one input report apart by its fields. r starts at the report ID
// when the descriptor has IDs.
static void hidp_decode(const hidp_t *h, const uint8_t *r, size_t len, hidp_report_t *out) {
    memset(out, 0, sizeof(*out));
    out->hat = -1;
    uint8_t id = 0;
    if (h->ids) {
        if (len < 1) {
            return;
        }
        id = r[0];
        r++;
        len--;
    }
    for (int i = 0; i < h->n; i++) {
        const hidp_field_t *f = &h->f[i];
        if (f->id != id) {
            continue;
        }
        if (out->kind == HIDP_OTHER && f->kind != HIDP_OTHER) {
            out->kind = f->kind;
        }
        if (f->page == 0x07) {                          // keyboard
            if (f->array) {
                for (int e = 0; e < f->count; e++) {
                    int32_t v = hidp_get(f, r, len, e);
                    uint32_t key = (uint32_t)(v - f->lmin) + f->usage;
                    if (v >= f->lmin && v <= f->lmax && key > 3 && key < 0xe0 && out->nkeys < 6) {
                        out->keys[out->nkeys++] = (uint8_t)key;
                    }
                }
            } else {
                for (int e = 0; e < f->count; e++) {
                    uint32_t key = f->usage + e;
                    if (!hidp_get(f, r, len, e)) {
                        continue;
                    }
                    if (key >= 0xe0 && key <= 0xe7) {
                        out->modifiers |= 1 << (key - 0xe0);
                    } else if (key > 3 && out->nkeys < 6) {
                        out->keys[out->nkeys++] = (uint8_t)key;
                    }
                }
            }
        } else if (f->page == 0x09) {                   // buttons
            for (int e = 0; e < f->count; e++) {
                int32_t v = hidp_get(f, r, len, e);
                uint32_t n = f->array ? (uint32_t)(v - f->lmin) + f->usage : (uint32_t)(f->usage + e);
                if ((f->array ? v >= f->lmin && n > 0 : v != 0) && n >= 1 && n <= 32) {
                    out->buttons |= 1u << (n - 1);
                }
            }
        } else if (f->page == 0x0c) {                   // consumer
            for (int e = 0; e < f->count && out->usage == 0; e++) {
                int32_t v = hidp_get(f, r, len, e);
                if (f->array) {
                    if (v >= f->lmin && v <= f->lmax && v != 0) {
                        out->usage = (uint16_t)((v - f->lmin) + f->usage);
                    }
                } else if (v) {
                    out->usage = (uint16_t)(f->usage + e);
                }
            }
        } else if (f->page == 0x01 && !f->array) {      // generic desktop
            int32_t v = hidp_get(f, r, len, 0);
            switch (f->usage) {
                case 0x30: out->x = v; out->ax = hidp_axis(f, v); break;
                case 0x31: out->y = v; out->ay = hidp_axis(f, v); break;
                case 0x32: out->az = hidp_axis(f, v); break;
                case 0x35: out->arz = hidp_axis(f, v); break;
                case 0x38: out->wheel = v; break;
                case 0x39: {                            // hat switch
                    int32_t span = f->lmax - f->lmin;
                    int32_t k = v - f->lmin;
                    if (span >= 0 && k >= 0 && k <= span && (span == 7 || span == 3)) {
                        out->hat = span == 3 ? k * 2 : k;
                    }
                    break;
                }
            }
        }
    }
}

#endif
