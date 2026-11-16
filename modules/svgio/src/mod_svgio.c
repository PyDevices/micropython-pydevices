// svgio for MicroPython: SVG files rendered to RGB565 and an alpha mask.
//
//   d = svgio.SvgDecoder()
//   w, h = d.open("/icons/wifi.svg", width=48)   # height follows the aspect ratio
//   d.decode(pixels, alpha)                       # colour and coverage
//   d.decode(fb_buf, x=10, y=10, stride=320)      # or blended over a framebuffer
//
//   d = svgio.SvgDecoder(memory=256 * 1024)       # its own working memory
//   d.decode(pixels, alpha, rows=(0, 120))        # a band of the picture
//
// The engine is NanoSVG (svgio_nanosvg.c). On CPython, pydevices-desktop's
// svgio.py gives the same API over resvg.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/builtin.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/objstr.h"

#include "svgio_core.h"

// Defined by micropython.mk / micropython.cmake only when LVGL is in the same
// build: svgio then also registers an LVGL image decoder (lvgl_decoder.c).
#ifndef SVGIO_LVGL_DECODER
#define SVGIO_LVGL_DECODER 0
#endif
#if SVGIO_LVGL_DECODER
#include "lvgl_decoder.h"
#endif

#define SVGIO_MAX_SIDE 32767

typedef struct {
    mp_obj_base_t base;
    svgio_image_t *image;   // the parsed SVG, kept for any number of decodes
    svgio_prepared_t *prepared;     // its outlines at scale, from the first decode
    float scale;
    int width, height;      // the size open() chose, in pixels
    svgio_arena_t mem;      // what it holds, and its working memory if it was given some
} svgio_decoder_obj_t;

// The least working memory worth having: a 24 px icon needs more.
#define SVGIO_MEMORY_MIN 4096

// SvgDecoder(*, memory=None)
static mp_obj_t svgio_decoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_memory };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_memory, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_int_t memory = 0;
    if (a[ARG_memory].u_obj != mp_const_none) {
        memory = mp_obj_get_int(a[ARG_memory].u_obj);
        if (memory < SVGIO_MEMORY_MIN || memory > 0x7FFFFFF8) {
            mp_raise_ValueError(MP_ERROR_TEXT("svgio: memory must be at least 4096 bytes"));
        }
        memory &= ~(mp_int_t)7;
    }
    // With a finaliser, so a block from outside the heap goes back when the
    // decoder is collected without close().
    svgio_decoder_obj_t *self = mp_obj_malloc_with_finaliser(svgio_decoder_obj_t, type);
    self->image = NULL;
    self->prepared = NULL;
    self->scale = 0;
    self->width = self->height = 0;
    self->mem.base = NULL;
    self->mem.outside = false;
    self->mem.spilled = NULL;
    self->mem.size = 0;
    svgio_arena_reset(&self->mem);
    if (memory != 0) {
        // One block, asked for once: from outside the heap where there is
        // such memory, so the collector never reads it. Otherwise a block of
        // the heap, which the decoder keeps alive by pointing at its head.
        uint8_t *base = svgio_outside_alloc(memory);
        self->mem.outside = base != NULL;
        self->mem.base = base != NULL ? base : m_new(uint8_t, memory);
        self->mem.size = (uint32_t)memory;
        svgio_arena_reset(&self->mem);
    }
    return MP_OBJ_FROM_PTR(self);
}

// close(): the decoder lets go of the SVG and of its working memory now; it
// works on as a decoder without any. Also what collecting a decoder does,
// where only memory from outside the heap is given back: the collector takes
// the rest itself, and may already have.
static mp_obj_t svgio_decoder_close(mp_obj_t self_in) {
    svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->mem.outside) {
        svgio_mem_use(&self->mem);
        svgio_prepared_free(self->prepared);
        svgio_image_free(self->image);
        svgio_mem_use(NULL);
    }
    // Outside the heap, the SVG is in the block and in what spilled from it,
    // and both go back whole.
    self->prepared = NULL;
    self->image = NULL;
    self->width = self->height = 0;
    self->mem.size = 0;
    svgio_arena_reset(&self->mem);
    if (self->mem.outside) {
        svgio_outside_free(self->mem.base);
    }
    self->mem.base = NULL;
    self->mem.outside = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_decoder_close_obj, svgio_decoder_close);

static mp_obj_t svgio_decoder_del(mp_obj_t self_in) {
    svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->mem.outside) {
        svgio_decoder_close(self_in);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_decoder_del_obj, svgio_decoder_del);

// The whole file as bytes, from a path or a stream.
static mp_obj_t svgio_read_all(mp_obj_t src) {
    mp_obj_t f = src;
    bool close = false;
    if (mp_obj_is_str(src)) {
        mp_obj_t args[2] = { src, MP_OBJ_NEW_QSTR(MP_QSTR_rb) };
        f = mp_builtin_open(2, args, (mp_map_t *)&mp_const_empty_map);
        close = true;
    }
    mp_obj_t data = mp_call_function_0(mp_load_attr(f, MP_QSTR_read));
    if (close) {
        mp_stream_close(f);
    }
    return data;
}

static int svgio_side(float v) {
    int n = (int)(v + 0.5f);
    return n < 1 ? 1 : n;
}

// An image open() parsed and then can't use goes back to the decoder's memory.
static void svgio_decoder_drop(svgio_decoder_obj_t *self, svgio_image_t *image) {
    svgio_mem_use(&self->mem);
    svgio_image_free(image);
    svgio_mem_use(NULL);
}

// open(source, *, width=None, height=None, scale=None) -> (width, height)
static mp_obj_t svgio_decoder_open(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_source, ARG_width, ARG_height, ARG_scale };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_width, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_height, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_scale, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(a[ARG_self].u_obj);
    const bool by_w = a[ARG_width].u_obj != mp_const_none;
    const bool by_h = a[ARG_height].u_obj != mp_const_none;
    const bool by_s = a[ARG_scale].u_obj != mp_const_none;
    if (by_w + by_h + by_s > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("give one of width, height and scale"));
    }

    svgio_mem_use(&self->mem);
    svgio_prepared_free(self->prepared);
    self->prepared = NULL;
    svgio_image_free(self->image);
    self->image = NULL;
    svgio_arena_reset(&self->mem);
    svgio_mem_use(NULL);
    self->width = self->height = 0;

    mp_obj_t source = a[ARG_source].u_obj;
    mp_buffer_info_t b;
    // A str has the buffer protocol too, so test for a path first.
    if (mp_obj_is_str(source) || !mp_get_buffer(source, &b, MP_BUFFER_READ)) {
        source = svgio_read_all(source);
        mp_get_buffer_raise(source, &b, MP_BUFFER_READ);
    }
    // The parser writes into its text, and wants it NUL-terminated.
    svgio_mem_reset();
    svgio_mem_use(&self->mem);
    char *text;
    const char *why = NULL;
    if (svgio_is_compact(b.buf, b.len)) {
        // A drawing in the compact form: the text it stands for, then the same parse.
        size_t n;
        text = svgio_expand(b.buf, b.len, &n, &why);
    } else {
        text = svgio_mem_alloc(b.len + 1);
        if (text != NULL) {
            memcpy(text, b.buf, b.len);
            text[b.len] = '\0';
        }
    }
    float sw = 0, sh = 0;
    svgio_image_t *image = NULL;
    if (text != NULL) {
        image = svgio_parse(text, &sw, &sh);
        svgio_mem_free(text);
    }
    if (why != NULL && strncmp(why, "not enough", 10) != 0) {
        svgio_mem_use(NULL);
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("svgio: %s"), why);
    }
    if (image != NULL && !(sw > 0 && sh > 0)) {
        svgio_image_free(image);
        svgio_mem_use(NULL);
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: not an SVG with a size (no <svg> with width and height, or viewBox)"));
    }
    svgio_mem_use(NULL);
    if (image == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("svgio: not enough memory to parse the SVG"));
    }

    float scale = 1.0f;
    if (by_w) {
        scale = (float)mp_obj_get_int(a[ARG_width].u_obj) / sw;
    } else if (by_h) {
        scale = (float)mp_obj_get_int(a[ARG_height].u_obj) / sh;
    } else if (by_s) {
        scale = (float)mp_obj_get_float(a[ARG_scale].u_obj);
    }
    const float fw = sw * scale, fh = sh * scale;
    if (!(scale > 0) || fw > SVGIO_MAX_SIDE || fh > SVGIO_MAX_SIDE) {
        svgio_decoder_drop(self, image);
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: the size must come to 1..32767 pixels a side"));
    }
    self->image = image;
    self->scale = scale;
    self->width = by_w ? mp_obj_get_int(a[ARG_width].u_obj) : svgio_side(fw);
    self->height = by_h ? mp_obj_get_int(a[ARG_height].u_obj) : svgio_side(fh);
    if (self->width < 1 || self->height < 1) {
        svgio_decoder_drop(self, self->image);
        self->image = NULL;
        self->width = self->height = 0;
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: the size must come to 1..32767 pixels a side"));
    }
    mp_obj_t wh[2] = { MP_OBJ_NEW_SMALL_INT(self->width), MP_OBJ_NEW_SMALL_INT(self->height) };
    return mp_obj_new_tuple(2, wh);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(svgio_decoder_open_obj, 2, svgio_decoder_open);

// Where decode() puts each strip.
typedef struct {
    uint16_t *pixels;       // NULL for the mask alone
    uint8_t *alpha;         // NULL to blend over pixels
    mp_int_t x, y, stride;
} svgio_target_t;

static void svgio_put_rows(void *ctx, const uint8_t *rgba, int y, int rows, int width) {
    svgio_target_t *t = ctx;
    for (int r = 0; r < rows; r++) {
        size_t at = (size_t)(t->y + y + r) * t->stride + t->x;
        svgio_rgba_to_565(rgba + (size_t)r * width * 4, width,
            t->pixels ? t->pixels + at : NULL, t->alpha ? t->alpha + at : NULL);
    }
}

static void svgio_check_room(const char *what, size_t have, size_t need) {
    if (have < need) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%s has %d bytes; it needs %d"), what, (int)have, (int)need);
    }
}

// decode(target, alpha=None, *, x=0, y=0, stride=None, rows=None)
static mp_obj_t svgio_decoder_decode(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_target, ARG_alpha, ARG_x, ARG_y, ARG_stride, ARG_rows };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_target, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_alpha, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_rows, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(a[ARG_self].u_obj);
    if (self->image == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("decode() without open()"));
    }
    // The rows to draw, of the picture's own: all of them, or (first, end).
    mp_int_t y0 = 0, y1 = self->height;
    if (a[ARG_rows].u_obj != mp_const_none) {
        size_t n;
        mp_obj_t *pair;
        mp_obj_get_array(a[ARG_rows].u_obj, &n, &pair);
        if (n == 2) {
            y0 = mp_obj_get_int(pair[0]);
            y1 = mp_obj_get_int(pair[1]);
        }
        if (n != 2 || y0 < 0 || y0 > y1 || y1 > self->height) {
            mp_raise_ValueError(MP_ERROR_TEXT("rows is (first, end) within the image's height"));
        }
    }
    const mp_int_t w = self->width, h = self->height;
    const mp_int_t x = a[ARG_x].u_int, y = a[ARG_y].u_int;
    const mp_int_t stride = a[ARG_stride].u_obj == mp_const_none ? w : mp_obj_get_int(a[ARG_stride].u_obj);
    if (x < 0 || y < 0 || x + w > stride) {
        mp_raise_ValueError(MP_ERROR_TEXT("the image doesn't fit across the target at that x and stride"));
    }
    // The target need only reach the last row drawn.
    const size_t pixels = y1 > y0 ? (size_t)(y + y1 - 1) * stride + x + w : 0;
    svgio_target_t t = { NULL, NULL, x, y, stride };
    mp_buffer_info_t buf;
    if (a[ARG_target].u_obj != mp_const_none) {
        mp_get_buffer_raise(a[ARG_target].u_obj, &buf, MP_BUFFER_WRITE);
        svgio_check_room("target", buf.len, pixels * 2);
        t.pixels = buf.buf;
    }
    if (a[ARG_alpha].u_obj != mp_const_none) {
        mp_get_buffer_raise(a[ARG_alpha].u_obj, &buf, MP_BUFFER_WRITE);
        svgio_check_room("alpha", buf.len, pixels);
        t.alpha = buf.buf;
    }
    if (t.pixels == NULL && t.alpha == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("give a target, an alpha buffer, or both"));
    }
    if (y1 == y0) {
        return mp_const_none;
    }

    int rows = SVGIO_STRIP_BYTES / (w * 4);
    rows = rows < 1 ? 1 : rows > h ? h : rows;
    const size_t strip_len = (size_t)w * rows * 4;
    svgio_mem_reset();
    svgio_mem_use(&self->mem);
    #if SVGIO_BENCH
    const bool prepare = svgio_opts & 4;
    #else
    const bool prepare = true;
    #endif
    const char *why = NULL;
    if (prepare && self->prepared == NULL) {
        // Flattened once here, the outlines serve every strip, every band and
        // every later decode() until the next open().
        self->prepared = svgio_prepare(self->image, self->scale);
        if (self->prepared == NULL) {
            why = "not enough memory to render";
        }
    }
    uint8_t *strip = why == NULL ? svgio_mem_alloc(strip_len) : NULL;
    if (why == NULL && strip == NULL) {
        why = "not enough memory to render";
    }
    if (why == NULL) {
        why = svgio_render(self->image, prepare ? self->prepared : NULL, self->scale, w, h, y0, y1,
            strip, rows, svgio_put_rows, &t);
    }
    svgio_mem_free(strip);
    svgio_mem_use(NULL);
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_MemoryError, MP_ERROR_TEXT("svgio: %s"), why);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(svgio_decoder_decode_obj, 2, svgio_decoder_decode);

static void svgio_decoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;                         // width and height can't be set
    }
    if (attr == MP_QSTR_memory || attr == MP_QSTR_memory_peak) {
        svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
        dest[0] = mp_obj_new_int_from_uint(attr == MP_QSTR_memory ? self->mem.size : self->mem.peak);
    } else if (attr == MP_QSTR_width || attr == MP_QSTR_height) {
        svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
        if (self->image == NULL) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("width and height need a successful open()"));
        }
        dest[0] = MP_OBJ_NEW_SMALL_INT(attr == MP_QSTR_height ? self->height : self->width);
    } else {
        dest[1] = MP_OBJ_SENTINEL;      // the methods
    }
}

static const mp_rom_map_elem_t svgio_decoder_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&svgio_decoder_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&svgio_decoder_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&svgio_decoder_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&svgio_decoder_del_obj) },
};
static MP_DEFINE_CONST_DICT(svgio_decoder_locals, svgio_decoder_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    svgio_decoder_type,
    MP_QSTR_SvgDecoder,
    MP_TYPE_FLAG_NONE,
    make_new, svgio_decoder_make_new,
    attr, svgio_decoder_attr,
    locals_dict, &svgio_decoder_locals
    );

// --- Buffer --------------------------------------------------------------------
//
// Buffer(size): bytes for pictures, from outside the MicroPython heap where
// the port has memory there. The collector reads every block of the heap it
// keeps, so a few megabytes of sprites in bytearrays make each collection
// take a few hundred milliseconds on a board; a Buffer costs it nothing. It
// has the buffer protocol, so decode() draws into it or into a memoryview of
// a part of it, and it isn't cleared.
//
// A memoryview holds a pointer to the bytes, not to the Buffer, and the
// collector follows pointers only into its own heap: it can collect a Buffer
// whose views are still in use. So collecting a Buffer never gives its bytes
// back to the system. They are parked, for the next Buffer that fits to
// take, and a view left over from the old one goes on reading and writing
// memory svgio owns. Only close() gives bytes back, as does running out:
// when the system has none left, what is parked is returned to it.

typedef struct {
    mp_obj_base_t base;
    uint8_t *buf;
    size_t len;
    size_t room;        // the bytes buf was taken with, len or more
    bool outside;       // buf came from svgio_outside_alloc()
} svgio_buffer_obj_t;

// The parked blocks. The list is kept here, not in the blocks, which a view
// may still write anywhere in. When it is full, one more collected Buffer's
// bytes are simply never used again.
#define SVGIO_PARKED_MAX 32
static struct {
    uint8_t *buf;
    size_t room;
} svgio_parked[SVGIO_PARKED_MAX];

// The smallest parked block of n bytes or more, off the list, unless it is
// more than twice what was asked for: a sprite shouldn't sit in a block that
// could hold a screen.
static uint8_t *svgio_unpark(size_t n, size_t *room) {
    int best = -1;
    for (int i = 0; i < SVGIO_PARKED_MAX; i++) {
        if (svgio_parked[i].buf != NULL && svgio_parked[i].room >= n
            && (best < 0 || svgio_parked[i].room < svgio_parked[best].room)) {
            best = i;
        }
    }
    if (best < 0 || svgio_parked[best].room / 2 > n) {
        return NULL;
    }
    uint8_t *buf = svgio_parked[best].buf;
    *room = svgio_parked[best].room;
    svgio_parked[best].buf = NULL;
    return buf;
}

static mp_obj_t svgio_buffer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    const mp_int_t n = mp_obj_get_int(args[0]);
    if (n < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: a Buffer has at least one byte"));
    }
    svgio_buffer_obj_t *self = mp_obj_malloc_with_finaliser(svgio_buffer_obj_t, type);
    self->len = 0;
    self->room = n;
    self->buf = svgio_unpark(n, &self->room);
    if (self->buf == NULL) {
        self->buf = svgio_outside_alloc(self->room);
    }
    if (self->buf == NULL) {
        // The system has no more: it gets back what is parked, and is asked again.
        for (int i = 0; i < SVGIO_PARKED_MAX; i++) {
            svgio_outside_free(svgio_parked[i].buf);
            svgio_parked[i].buf = NULL;
        }
        self->buf = svgio_outside_alloc(self->room);
    }
    self->outside = self->buf != NULL;
    if (self->buf == NULL) {
        // A port with no memory but the heap, where a view does keep its bytes.
        self->buf = m_new(uint8_t, n);
    }
    self->len = n;
    return MP_OBJ_FROM_PTR(self);
}

// close(): the bytes go back to the system now. A memoryview of them must not
// be used again.
static mp_obj_t svgio_buffer_close(mp_obj_t self_in) {
    svgio_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->outside) {
        svgio_outside_free(self->buf);
    }
    self->buf = NULL;
    self->len = 0;
    self->outside = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_buffer_close_obj, svgio_buffer_close);

// Collected without close(): a view of the bytes may be in use still, so they
// are parked, not given back.
static mp_obj_t svgio_buffer_del(mp_obj_t self_in) {
    svgio_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    for (int i = 0; self->outside && i < SVGIO_PARKED_MAX; i++) {
        if (svgio_parked[i].buf == NULL) {
            svgio_parked[i].buf = self->buf;
            svgio_parked[i].room = self->room;
            break;
        }
    }
    self->buf = NULL;
    self->len = 0;
    self->outside = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_buffer_del_obj, svgio_buffer_del);

static mp_int_t svgio_buffer_get_buffer(mp_obj_t self_in, mp_buffer_info_t *info, mp_uint_t flags) {
    (void)flags;
    svgio_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->buf == NULL) {
        return 1;       // closed
    }
    info->buf = self->buf;
    info->len = self->len;
    info->typecode = 'B';
    return 0;
}

static mp_obj_t svgio_buffer_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    svgio_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(self->len != 0);
        case MP_UNARY_OP_LEN:
            return mp_obj_new_int_from_uint(self->len);
        default:
            return MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t svgio_buffer_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&svgio_buffer_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&svgio_buffer_del_obj) },
};
static MP_DEFINE_CONST_DICT(svgio_buffer_locals, svgio_buffer_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    svgio_buffer_type,
    MP_QSTR_Buffer,
    MP_TYPE_FLAG_NONE,
    make_new, svgio_buffer_make_new,
    unary_op, svgio_buffer_unary_op,
    buffer, svgio_buffer_get_buffer,
    locals_dict, &svgio_buffer_locals
    );

// The LVGL decoder's last SVG (lvgl_decoder.c). Registered on every build, as
// the module's names are, because the CMake ports' QSTR pass can't see what
// lives only inside #if SVGIO_LVGL_DECODER.
MP_REGISTER_ROOT_POINTER(void *svgio_kept);

void **svgio_kept_slot(void) {
    return &MP_STATE_VM(svgio_kept);
}

// --- LVGL image decoder --------------------------------------------------------
//
// register_lvgl_decoder() and lvgl_decoders() are module attributes on every
// build, as jpegio's are: the API doesn't vanish when LVGL isn't built in, and
// the CMake ports' QSTR pass can't see a name that lives only inside the #if.

// register_lvgl_decoder() -> bool: True when this call added svgio's decoder
// to the running LVGL, False when it was already there. RuntimeError before
// lv.init(), and on a build without LVGL.
static mp_obj_t svgio_register_lvgl_decoder(void) {
    #if SVGIO_LVGL_DECODER
    if (svgio_lvgl_decoder_registered()) {
        return mp_const_false;
    }
    if (!svgio_lvgl_decoder_register()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("register_lvgl_decoder() needs an initialised LVGL: lv.init() first"));
    }
    return mp_const_true;
    #else
    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("svgio was built without its LVGL decoder: lvgl was not in the build"));
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio_register_lvgl_decoder_obj, svgio_register_lvgl_decoder);

// lvgl_decoders() -> tuple of str: LVGL's image decoders in the order it asks
// them; empty before lv.init() and on a build without LVGL.
static mp_obj_t svgio_lvgl_decoders(void) {
    #if SVGIO_LVGL_DECODER
    size_t n = 0;
    while (svgio_lvgl_decoder_name_at(n) != NULL) {
        n++;
    }
    mp_obj_tuple_t *names = MP_OBJ_TO_PTR(mp_obj_new_tuple(n, NULL));
    for (size_t i = 0; i < n; i++) {
        const char *name = svgio_lvgl_decoder_name_at(i);
        names->items[i] = mp_obj_new_str(name, strlen(name));
    }
    return MP_OBJ_FROM_PTR(names);
    #else
    return mp_const_empty_tuple;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio_lvgl_decoders_obj, svgio_lvgl_decoders);

#if SVGIO_LVGL_DECODER
// `import svgio` after lv.init() registers the decoder, so nothing more is
// needed; before lv.init() it does nothing.
static mp_obj_t svgio___init__(void) {
    svgio_lvgl_decoder_register();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio___init___obj, svgio___init__);
#endif

// _mem_peak() -> int: the most heap the parser or rasteriser held at once
// during the last open() or decode(), in bytes. For sizing, not for apps.
static mp_obj_t svgio_mem_peak_fn(void) {
    return mp_obj_new_int_from_uint(svgio_mem_peak());
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio_mem_peak_obj, svgio_mem_peak_fn);

// _outside() -> int: how many blocks of memory from outside the heap svgio
// holds now, in decoders and Buffers. For its tests.
static mp_obj_t svgio_outside_fn(void) {
    return mp_obj_new_int_from_uint(svgio_outside_held());
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio_outside_obj, svgio_outside_fn);

#if SVGIO_BENCH
// _opts(mask): which speedups run (svgio_core.h lists the bits), in a
// profiling build.
static mp_obj_t svgio_opts_fn(mp_obj_t mask) {
    svgio_opts = mp_obj_get_int(mask);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_opts_obj, svgio_opts_fn);

// _profile() -> (parse, prepare, edges, raster, unpremultiply, convert): the
// microseconds each phase took since the last call, in a profiling build.
static mp_obj_t svgio_profile_fn(void) {
    mp_obj_t t[SVGIO_PHASES];
    for (int i = 0; i < SVGIO_PHASES; i++) {
        t[i] = mp_obj_new_int_from_uint(svgio_phase_us[i]);
        svgio_phase_us[i] = 0;
    }
    return mp_obj_new_tuple(SVGIO_PHASES, t);
}
static MP_DEFINE_CONST_FUN_OBJ_0(svgio_profile_obj, svgio_profile_fn);
#endif

// expand(drawing) -> bytes: the SVG text a drawing in the compact form stands
// for, which is what open() parses when it is given one.
static mp_obj_t svgio_expand_fn(mp_obj_t drawing) {
    mp_buffer_info_t b;
    mp_get_buffer_raise(drawing, &b, MP_BUFFER_READ);
    size_t n = 0;
    const char *why = "not a drawing in svgio's compact form";
    svgio_mem_use(NULL);
    char *text = svgio_is_compact(b.buf, b.len) ? svgio_expand(b.buf, b.len, &n, &why) : NULL;
    if (text == NULL) {
        if (strncmp(why, "not enough", 10) == 0) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("svgio: not enough memory for the SVG text"));
        }
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("svgio: %s"), why);
    }
    // text is on the heap: if making the bytes raises, the collector takes it
    mp_obj_t out = mp_obj_new_bytes((const byte *)text, n);
    svgio_mem_free(text);
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svgio_expand_obj, svgio_expand_fn);

// svgio.__revision__: which svgio this firmware was built from (git describe).
#ifndef SVGIO_REVISION
#define SVGIO_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(svgio_revision_obj, SVGIO_REVISION);

static const mp_rom_map_elem_t svgio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_svgio) },
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&svgio_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_SvgDecoder), MP_ROM_PTR(&svgio_decoder_type) },
    { MP_ROM_QSTR(MP_QSTR_Buffer), MP_ROM_PTR(&svgio_buffer_type) },
    #if SVGIO_LVGL_DECODER
    { MP_ROM_QSTR(MP_QSTR___init__), MP_ROM_PTR(&svgio___init___obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_register_lvgl_decoder), MP_ROM_PTR(&svgio_register_lvgl_decoder_obj) },
    { MP_ROM_QSTR(MP_QSTR_lvgl_decoders), MP_ROM_PTR(&svgio_lvgl_decoders_obj) },
    { MP_ROM_QSTR(MP_QSTR_expand), MP_ROM_PTR(&svgio_expand_obj) },
    { MP_ROM_QSTR(MP_QSTR__mem_peak), MP_ROM_PTR(&svgio_mem_peak_obj) },
    { MP_ROM_QSTR(MP_QSTR__outside), MP_ROM_PTR(&svgio_outside_obj) },
    #if SVGIO_BENCH
    { MP_ROM_QSTR(MP_QSTR__opts), MP_ROM_PTR(&svgio_opts_obj) },
    { MP_ROM_QSTR(MP_QSTR__profile), MP_ROM_PTR(&svgio_profile_obj) },
    #endif
};
static MP_DEFINE_CONST_DICT(svgio_module_globals, svgio_module_globals_table);

const mp_obj_module_t svgio_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&svgio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_svgio, svgio_user_cmodule);
