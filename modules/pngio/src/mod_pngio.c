// pngio for MicroPython: PNG encode and decode, beside jpegio.
//
//   png = pngio.PngEncoder(level=1).encode(fb, 480, 270)        # bytes
//   d = pngio.PngDecoder(); w, h = d.open(png); d.decode(buf)   # RGB565 into buf
//
// The engine is pngio_core.c. Decoding inflates through MicroPython's own
// uzlib (the `deflate` module's), one scanline at a time. On CPython,
// pydevices-desktop's pngio.py gives the same API over Pillow.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/builtin.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "pngio_core.h"

#if MICROPY_PY_DEFLATE
#include "lib/uzlib/uzlib.h"
#endif

// --- PngEncoder ----------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    int level;
    uint8_t *work, *out;
    size_t work_len, out_len;
} pngio_encoder_obj_t;

static mp_obj_t pngio_encoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_level };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_level, MP_ARG_INT, { .u_int = 1 } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, a);
    if (a[ARG_level].u_int < 1 || a[ARG_level].u_int > 9) {
        mp_raise_ValueError(MP_ERROR_TEXT("level must be 1..9"));
    }
    pngio_encoder_obj_t *self = mp_obj_malloc(pngio_encoder_obj_t, type);
    self->level = a[ARG_level].u_int;
    self->work = self->out = NULL;
    self->work_len = self->out_len = 0;
    return MP_OBJ_FROM_PTR(self);
}

// encode(buffer, width, height, *, format=RGB565, stride=None, swap=False) -> bytes
static mp_obj_t pngio_encoder_encode(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_buffer, ARG_width, ARG_height, ARG_format, ARG_stride, ARG_swap };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = PNGIO_RGB565 } },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
        { MP_QSTR_swap, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    pngio_encoder_obj_t *self = MP_OBJ_TO_PTR(a[ARG_self].u_obj);
    mp_int_t w = a[ARG_width].u_int, h = a[ARG_height].u_int;
    int fmt = a[ARG_format].u_int;
    if (w < 1 || h < 1 || w > 32767 || h > 32767) {
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be 1..32767"));
    }
    if (fmt != PNGIO_RGB565 && fmt != PNGIO_GS8 && fmt != PNGIO_RGB888) {
        mp_raise_ValueError(MP_ERROR_TEXT("format must be RGB565, GS8 or RGB888"));
    }
    mp_int_t stride = a[ARG_stride].u_obj == mp_const_none ? w : mp_obj_get_int(a[ARG_stride].u_obj);
    if (stride < w) {
        mp_raise_ValueError(MP_ERROR_TEXT("stride must be at least width"));
    }
    mp_buffer_info_t src;
    mp_get_buffer_raise(a[ARG_buffer].u_obj, &src, MP_BUFFER_READ);
    const size_t need = ((size_t)stride * (h - 1) + w) * pngio_in_bpp(fmt);
    if (src.len < need) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("buffer has %d bytes; %dx%d needs %d"),
            (int)src.len, (int)w, (int)h, (int)need);
    }
    // scratch and output, kept between calls and grown as needed
    const size_t work = pngio_encode_work(w, h, fmt, self->level), cap = pngio_encode_bound(w, h, fmt);
    if (self->work_len < work) {
        self->work = m_renew(uint8_t, self->work, self->work_len, work);
        self->work_len = work;
    }
    if (self->out_len < cap) {
        self->out = m_renew(uint8_t, self->out, self->out_len, cap);
        self->out_len = cap;
    }
    size_t len = 0;
    MP_THREAD_GIL_EXIT();
    const char *why = pngio_encode(src.buf, w, h, stride, fmt, a[ARG_swap].u_bool, self->level,
        self->work, self->out, self->out_len, &len);
    MP_THREAD_GIL_ENTER();
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pngio: %s"), why);
    }
    return mp_obj_new_bytes(self->out, len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pngio_encoder_encode_obj, 4, pngio_encoder_encode);

static const mp_rom_map_elem_t pngio_encoder_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&pngio_encoder_encode_obj) },
};
static MP_DEFINE_CONST_DICT(pngio_encoder_locals, pngio_encoder_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    pngio_encoder_type,
    MP_QSTR_PngEncoder,
    MP_TYPE_FLAG_NONE,
    make_new, pngio_encoder_make_new,
    locals_dict, &pngio_encoder_locals
    );

// --- PngDecoder ----------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    mp_obj_t source;        // the PNG's bytes, held until decode()
    pngio_info_t info;
    bool opened;
} pngio_decoder_obj_t;

static mp_obj_t pngio_decoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    pngio_decoder_obj_t *self = mp_obj_malloc(pngio_decoder_obj_t, type);
    self->source = mp_const_none;
    self->opened = false;
    return MP_OBJ_FROM_PTR(self);
}

// The whole file as bytes, from a path or a stream.
static mp_obj_t pngio_read_all(mp_obj_t src) {
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

// open(source) -> (width, height): bytes-like, a path, or a binary stream
static mp_obj_t pngio_decoder_open(mp_obj_t self_in, mp_obj_t source) {
    pngio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->opened = false;
    mp_buffer_info_t b;
    if (!mp_get_buffer(source, &b, MP_BUFFER_READ)) {
        source = pngio_read_all(source);
        mp_get_buffer_raise(source, &b, MP_BUFFER_READ);
    }
    const char *why = pngio_parse(b.buf, b.len, &self->info);
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pngio: %s"), why);
    }
    self->source = source;
    self->opened = true;
    mp_obj_t wh[2] = { MP_OBJ_NEW_SMALL_INT(self->info.width), MP_OBJ_NEW_SMALL_INT(self->info.height) };
    return mp_obj_new_tuple(2, wh);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pngio_decoder_open_obj, pngio_decoder_open);

#if MICROPY_PY_DEFLATE
// The decompressor and where it is in the PNG's chunks, together: uzlib hands
// its source callback only source_read_data, and the callback has to move
// the decompressor's source on to the next IDAT.
typedef struct {
    uzlib_uncomp_t d;
    const uint8_t *png;
    size_t len, pos;
} pngio_inflate_t;

static int pngio_next_byte(void *data) {
    pngio_inflate_t *z = data;
    const uint8_t *p;
    size_t n;
    while (pngio_next_idat(z->png, z->len, &z->pos, &p, &n)) {
        if (n) {
            z->d.source = p + 1;
            z->d.source_limit = p + n;
            return p[0];
        }
    }
    return -1;
}

// Inflate exactly n bytes into dst; NULL, or why not.
static const char *pngio_inflate(pngio_inflate_t *z, uint8_t *dst, size_t n) {
    z->d.dest = dst;
    z->d.dest_limit = dst + n;
    while (z->d.dest < z->d.dest_limit) {
        int st = uzlib_uncompress_chksum(&z->d);
        if (st == UZLIB_DONE) {
            break;
        }
        if (st < 0) {
            return "PNG image data is corrupt";
        }
    }
    return z->d.dest == z->d.dest_limit ? NULL : "PNG image data ends early";
}

// After the last row: read on to the end of the stream, so uzlib checks its
// Adler-32 (a corrupt image can inflate to the right number of bytes).
static const char *pngio_inflate_finish(pngio_inflate_t *z) {
    uint8_t tail[16];
    for (int i = 0; i < 64; i++) {
        z->d.dest = tail;
        z->d.dest_limit = tail + sizeof(tail);
        int st = uzlib_uncompress_chksum(&z->d);
        if (st == UZLIB_DONE) {
            return NULL;
        }
        if (st < 0) {
            return "PNG image data is corrupt";
        }
    }
    return "PNG image data runs on past the image";
}
#endif

// decode(target, x=0, y=0, *, stride=None): native-order RGB565 into target
static mp_obj_t pngio_decoder_decode(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { ARG_self, ARG_target, ARG_x, ARG_y, ARG_stride };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_target, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_x, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_y, MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_rom_obj = MP_ROM_NONE } },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    pngio_decoder_obj_t *self = MP_OBJ_TO_PTR(a[ARG_self].u_obj);
    if (!self->opened) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("decode() without open()"));
    }
    #if !MICROPY_PY_DEFLATE
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("pngio: decoding needs the deflate module in this firmware"));
    #else
    const pngio_info_t *info = &self->info;
    const mp_int_t x = a[ARG_x].u_int, y = a[ARG_y].u_int;
    const mp_int_t stride = a[ARG_stride].u_obj == mp_const_none ? (mp_int_t)info->width : mp_obj_get_int(a[ARG_stride].u_obj);
    if (x < 0 || y < 0 || x + (mp_int_t)info->width > stride) {
        mp_raise_ValueError(MP_ERROR_TEXT("the image doesn't fit across the target at that x and stride"));
    }
    mp_buffer_info_t dst;
    mp_get_buffer_raise(a[ARG_target].u_obj, &dst, MP_BUFFER_WRITE);
    const size_t need = (((size_t)(y + info->height - 1)) * stride + x + info->width) * 2;
    if (dst.len < need) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("target has %d bytes; %dx%d at (%d, %d) with stride %d needs %d"),
            (int)dst.len, (int)info->width, (int)info->height, (int)x, (int)y, (int)stride, (int)need);
    }
    mp_buffer_info_t src;
    mp_get_buffer_raise(self->source, &src, MP_BUFFER_READ);

    pngio_inflate_t *z = m_new_obj(pngio_inflate_t);
    memset(z, 0, sizeof(*z));
    z->png = src.buf;
    z->len = src.len;
    z->d.source_read_data = z;
    z->d.source_read_cb = pngio_next_byte;
    const char *why = NULL;
    int wbits;
    if (uzlib_parse_zlib_gzip_header(&z->d, &wbits) != UZLIB_HEADER_ZLIB) {
        why = "PNG image data has no zlib header";
    }
    const size_t line = 1 + info->rowbytes;
    uint8_t *window = m_new(uint8_t, 32768);
    uint8_t *rows = m_new(uint8_t, 2 * line);
    uint8_t *cur = rows, *prev = rows + line;
    memset(prev, 0, line);
    if (why == NULL) {
        uzlib_uncompress_init(&z->d, window, 32768);
        uint16_t *out = (uint16_t *)dst.buf;
        for (uint32_t r = 0; r < info->height && why == NULL; r++) {
            why = pngio_inflate(z, cur, line);
            if (why == NULL) {
                why = pngio_unfilter(cur[0], cur + 1, prev + 1, info->rowbytes, info->bpp);
            }
            if (why == NULL) {
                pngio_row_to_565(info, cur + 1, out + (size_t)(y + r) * stride + x, info->width);
                uint8_t *t = prev;
                prev = cur;
                cur = t;
            }
        }
        if (why == NULL) {
            why = pngio_inflate_finish(z);
        }
    }
    m_del(uint8_t, rows, 2 * line);
    m_del(uint8_t, window, 32768);
    m_del_obj(pngio_inflate_t, z);
    self->opened = false;
    self->source = mp_const_none;
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("pngio: %s"), why);
    }
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(pngio_decoder_decode_obj, 2, pngio_decoder_decode);

static mp_obj_t pngio_decoder_attr_size(mp_obj_t self_in, int which) {
    pngio_decoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->info.width == 0) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("width needs a successful open()"));
    }
    return MP_OBJ_NEW_SMALL_INT(which ? self->info.height : self->info.width);
}

static void pngio_decoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;                         // width and height can't be set
    }
    if (attr == MP_QSTR_width || attr == MP_QSTR_height) {
        dest[0] = pngio_decoder_attr_size(self_in, attr == MP_QSTR_height);
    } else {
        dest[1] = MP_OBJ_SENTINEL;      // the methods
    }
}

static const mp_rom_map_elem_t pngio_decoder_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&pngio_decoder_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&pngio_decoder_decode_obj) },
};
static MP_DEFINE_CONST_DICT(pngio_decoder_locals, pngio_decoder_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    pngio_decoder_type,
    MP_QSTR_PngDecoder,
    MP_TYPE_FLAG_NONE,
    make_new, pngio_decoder_make_new,
    attr, pngio_decoder_attr,
    locals_dict, &pngio_decoder_locals
    );

static const mp_rom_map_elem_t pngio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pngio) },
    { MP_ROM_QSTR(MP_QSTR_PngEncoder), MP_ROM_PTR(&pngio_encoder_type) },
    { MP_ROM_QSTR(MP_QSTR_PngDecoder), MP_ROM_PTR(&pngio_decoder_type) },
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(PNGIO_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_GS8), MP_ROM_INT(PNGIO_GS8) },
    { MP_ROM_QSTR(MP_QSTR_RGB888), MP_ROM_INT(PNGIO_RGB888) },
};
static MP_DEFINE_CONST_DICT(pngio_module_globals, pngio_module_globals_table);

const mp_obj_module_t pngio_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pngio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pngio, pngio_user_cmodule);
