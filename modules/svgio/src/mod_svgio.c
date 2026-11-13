// svgio for MicroPython: SVG files rendered to RGB565 and an alpha mask.
//
//   d = svgio.SvgDecoder()
//   w, h = d.open("/icons/wifi.svg", width=48)   # height follows the aspect ratio
//   d.decode(pixels, alpha)                       # colour and coverage
//   d.decode(fb_buf, x=10, y=10, stride=320)      # or blended over a framebuffer
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
    float scale;
    int width, height;      // the size open() chose, in pixels
} svgio_decoder_obj_t;

static mp_obj_t svgio_decoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    svgio_decoder_obj_t *self = mp_obj_malloc(svgio_decoder_obj_t, type);
    self->image = NULL;
    self->scale = 0;
    self->width = self->height = 0;
    return MP_OBJ_FROM_PTR(self);
}

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

    svgio_image_free(self->image);
    self->image = NULL;
    self->width = self->height = 0;

    mp_obj_t source = a[ARG_source].u_obj;
    mp_buffer_info_t b;
    // A str has the buffer protocol too, so test for a path first.
    if (mp_obj_is_str(source) || !mp_get_buffer(source, &b, MP_BUFFER_READ)) {
        source = svgio_read_all(source);
        mp_get_buffer_raise(source, &b, MP_BUFFER_READ);
    }
    // The parser writes into its text, and wants it NUL-terminated.
    char *text = m_new(char, b.len + 1);
    memcpy(text, b.buf, b.len);
    text[b.len] = '\0';
    float sw = 0, sh = 0;
    svgio_mem_reset();
    svgio_image_t *image = svgio_parse(text, &sw, &sh);
    m_del(char, text, b.len + 1);
    if (image == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("svgio: not enough memory to parse the SVG"));
    }
    if (!(sw > 0 && sh > 0)) {
        svgio_image_free(image);
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: not an SVG with a size (no <svg> with width and height, or viewBox)"));
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
        svgio_image_free(image);
        mp_raise_ValueError(MP_ERROR_TEXT("svgio: the size must come to 1..32767 pixels a side"));
    }
    self->image = image;
    self->scale = scale;
    self->width = by_w ? mp_obj_get_int(a[ARG_width].u_obj) : svgio_side(fw);
    self->height = by_h ? mp_obj_get_int(a[ARG_height].u_obj) : svgio_side(fh);
    if (self->width < 1 || self->height < 1) {
        svgio_image_free(self->image);
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

// decode(target, alpha=None, *, x=0, y=0, stride=None)
static mp_obj_t svgio_decoder_decode(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_target, ARG_alpha, ARG_x, ARG_y, ARG_stride };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_target, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_alpha, MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    svgio_decoder_obj_t *self = MP_OBJ_TO_PTR(a[ARG_self].u_obj);
    if (self->image == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("decode() without open()"));
    }
    const mp_int_t w = self->width, h = self->height;
    const mp_int_t x = a[ARG_x].u_int, y = a[ARG_y].u_int;
    const mp_int_t stride = a[ARG_stride].u_obj == mp_const_none ? w : mp_obj_get_int(a[ARG_stride].u_obj);
    if (x < 0 || y < 0 || x + w > stride) {
        mp_raise_ValueError(MP_ERROR_TEXT("the image doesn't fit across the target at that x and stride"));
    }
    const size_t pixels = (size_t)(y + h - 1) * stride + x + w;
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

    int rows = SVGIO_STRIP_BYTES / (w * 4);
    rows = rows < 1 ? 1 : rows > h ? h : rows;
    const size_t strip_len = (size_t)w * rows * 4;
    uint8_t *strip = m_new_maybe(uint8_t, strip_len);
    if (strip == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("svgio: not enough memory to render"));
    }
    svgio_mem_reset();
    const char *why = svgio_render(self->image, self->scale, w, h, strip, rows, svgio_put_rows, &t);
    m_del(uint8_t, strip, strip_len);
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
    if (attr == MP_QSTR_width || attr == MP_QSTR_height) {
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

// svgio.__revision__: which svgio this firmware was built from (git describe).
#ifndef SVGIO_REVISION
#define SVGIO_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(svgio_revision_obj, SVGIO_REVISION);

static const mp_rom_map_elem_t svgio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_svgio) },
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&svgio_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_SvgDecoder), MP_ROM_PTR(&svgio_decoder_type) },
    #if SVGIO_LVGL_DECODER
    { MP_ROM_QSTR(MP_QSTR___init__), MP_ROM_PTR(&svgio___init___obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_register_lvgl_decoder), MP_ROM_PTR(&svgio_register_lvgl_decoder_obj) },
    { MP_ROM_QSTR(MP_QSTR_lvgl_decoders), MP_ROM_PTR(&svgio_lvgl_decoders_obj) },
    { MP_ROM_QSTR(MP_QSTR__mem_peak), MP_ROM_PTR(&svgio_mem_peak_obj) },
};
static MP_DEFINE_CONST_DICT(svgio_module_globals, svgio_module_globals_table);

const mp_obj_module_t svgio_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&svgio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_svgio, svgio_user_cmodule);
