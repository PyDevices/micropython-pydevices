// jpegio.JpegEncoder: RGB565 or 8-bit grayscale buffers to baseline JPEG.
//
//   enc = jpegio.JpegEncoder(quality=80, subsampling=420)
//   data = enc.encode(buf, width, height)                 # bytes
//   data = enc.encode(buf, w, h, format=jpegio.GRAY, stride=row_bytes, swap=False)
//   fast = jpegio.JpegEncoder(70, exact=False)   # the P4 engine's own RGB565 read
//
// Every port encodes in software (jpegenc/, stb_image_write's writer). On the
// ESP32-P4 the hardware JPEG engine does it instead (jpegio_hw.c), unless
// hardware=False; hardware=True insists on it and raises where there is none.
// RGB565 is native-order (what JpegDecoder.decode() writes) unless swap=True,
// for the byte-swapped order many SPI displays keep.
//
// Other C modules encode through jpegio_encode() (jpegio.h): cameraif's
// capture_jpeg() is one.

#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "jpegio.h"
#include "jpegio_hw.h"
#include "jpegenc/jpegenc.h"

typedef struct _jpegio_jpegencoder_obj_t {
    mp_obj_base_t base;
    uint8_t quality;
    bool subsample;     // 4:2:0, else 4:4:4
    int8_t hardware;    // -1 auto, 0 never, 1 required
    bool exact;         // widen RGB565 as a display does before the engine sees it
    bool last_hw;       // the last encode() used the hardware engine
} jpegio_jpegencoder_obj_t;

// --- the software path -------------------------------------------------------

typedef struct {
    const uint8_t *pixels;
    size_t stride;      // bytes per row
    int format;
    bool swap;
} jpegio_src_t;

static void jpegio_fetch(const void *src_in, int row, int col, float *r, float *g, float *b) {
    const jpegio_src_t *src = src_in;
    const uint8_t *p = src->pixels + (size_t)row * src->stride;
    if (src->format == JPEGIO_FORMAT_GRAY) {
        *r = *g = *b = p[col];
        return;
    }
    p += 2 * (size_t)col;
    uint16_t v = src->swap ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)(p[0] | (p[1] << 8));
    uint8_t r5 = v >> 11, g6 = (v >> 5) & 0x3F, b5 = v & 0x1F;
    // Replicate the top bits so full scale is 255, as a display shows it.
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static void jpegio_write_vstr(void *ctx, void *data, int size) {
    vstr_add_strn((vstr_t *)ctx, (const char *)data, (size_t)size);
}

static mp_obj_t jpegio_encode_sw(const jpegio_src_t *src, int width, int height, int quality, bool subsample) {
    vstr_t vstr;
    vstr_init(&vstr, 1024 + (size_t)width * height / 8);
    if (!jpegenc_write(jpegio_write_vstr, &vstr, width, height, jpegio_fetch, src, quality, subsample)) {
        vstr_clear(&vstr);
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be 1..65535"));
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}

// --- shared by Python and C callers -----------------------------------------

MP_NORETURN static void jpegio_no_hw(void) {
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no hardware JPEG encoder on this chip"));
}

static void jpegio_check(const void *pixels, size_t len, int width, int height, size_t stride, int format) {
    if (width < 1 || height < 1 || width > 65535 || height > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be 1..65535"));
    }
    size_t row = (size_t)width * (format == JPEGIO_FORMAT_GRAY ? 1 : 2);
    if (stride < row) {
        mp_raise_ValueError(MP_ERROR_TEXT("stride is shorter than a row"));
    }
    if (len < stride * (size_t)(height - 1) + row) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too small for width x height"));
    }
    (void)pixels;
}

mp_obj_t jpegio_encode(const void *pixels, size_t len, int width, int height, size_t stride,
    int format, bool swap, int quality, bool subsample, int hardware, bool exact, bool *used_hw) {
    if (quality < 1 || quality > 100) {
        mp_raise_ValueError(MP_ERROR_TEXT("quality must be 1..100"));
    }
    jpegio_check(pixels, len, width, height, stride, format);
    if (used_hw) {
        *used_hw = false;
    }
    // JPEGIO_HW is tested as a constant, not with #if: see jpegio_hw.h.
    if (JPEGIO_HW && hardware != 0) {
        mp_obj_t out = jpegio_hw_encode(pixels, width, height, stride, format, swap, quality, subsample, exact);
        if (out != MP_OBJ_NULL) {
            if (used_hw) {
                *used_hw = true;
            }
            return out;
        }
        if (hardware > 0) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("the JPEG engine could not encode this image"));
        }
    } else if (hardware > 0) {
        jpegio_no_hw();
    }
    jpegio_src_t src = { pixels, stride, format, swap };
    return jpegio_encode_sw(&src, width, height, quality, subsample);
}

// --- jpegio.JpegEncoder ------------------------------------------------------

static mp_obj_t jpegio_jpegencoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_quality, ARG_subsampling, ARG_hardware, ARG_exact };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_quality, MP_ARG_INT, {.u_int = 80} },
        { MP_QSTR_subsampling, MP_ARG_INT, {.u_int = 420} },
        { MP_QSTR_hardware, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_exact, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    mp_int_t q = args[ARG_quality].u_int;
    if (q < 1 || q > 100) {
        mp_raise_ValueError(MP_ERROR_TEXT("quality must be 1..100"));
    }
    mp_int_t ss = args[ARG_subsampling].u_int;
    if (ss != 420 && ss != 444) {
        mp_raise_ValueError(MP_ERROR_TEXT("subsampling must be 420 or 444"));
    }
    jpegio_jpegencoder_obj_t *self = mp_obj_malloc(jpegio_jpegencoder_obj_t, type);
    self->quality = (uint8_t)q;
    self->subsample = ss == 420;
    mp_obj_t hw = args[ARG_hardware].u_obj;
    self->hardware = hw == mp_const_none ? -1 : (mp_obj_is_true(hw) ? 1 : 0);
    if (!JPEGIO_HW && self->hardware > 0) {
        jpegio_no_hw();
    }
    self->exact = args[ARG_exact].u_bool;
    self->last_hw = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t jpegio_jpegencoder_encode(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    jpegio_jpegencoder_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    enum { ARG_buffer, ARG_width, ARG_height, ARG_format, ARG_stride, ARG_swap };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_buffer, MP_ARG_OBJ | MP_ARG_REQUIRED, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_width, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_INT | MP_ARG_REQUIRED, {.u_int = 0} },
        { MP_QSTR_format, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = JPEGIO_FORMAT_RGB565} },
        { MP_QSTR_stride, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_swap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int format = args[ARG_format].u_int;
    if (format != JPEGIO_FORMAT_RGB565 && format != JPEGIO_FORMAT_GRAY) {
        mp_raise_ValueError(MP_ERROR_TEXT("format must be RGB565 or GRAY"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &buf, MP_BUFFER_READ);
    mp_int_t w = args[ARG_width].u_int, h = args[ARG_height].u_int;
    size_t stride = (size_t)(w > 0 ? w : 0) * (format == JPEGIO_FORMAT_GRAY ? 1 : 2);
    if (args[ARG_stride].u_obj != mp_const_none) {
        mp_int_t s = mp_obj_get_int(args[ARG_stride].u_obj);
        if (s < 1) {
            mp_raise_ValueError(MP_ERROR_TEXT("stride must be positive"));
        }
        stride = (size_t)s;
    }
    bool used_hw = false;
    mp_obj_t out = jpegio_encode(buf.buf, buf.len, (int)w, (int)h, stride, format, args[ARG_swap].u_bool,
        self->quality, self->subsample, self->hardware, self->exact, &used_hw);
    self->last_hw = used_hw;
    return out;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(jpegio_jpegencoder_encode_obj, 4, jpegio_jpegencoder_encode);

static void jpegio_jpegencoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    jpegio_jpegencoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_quality) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->quality);
    } else if (attr == MP_QSTR_subsampling) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->subsample ? 420 : 444);
    } else if (attr == MP_QSTR_hardware) {
        // Whether the last encode() used the hardware engine.
        dest[0] = mp_obj_new_bool(self->last_hw);
    } else {
        dest[1] = MP_OBJ_SENTINEL;  // methods
    }
}

static const mp_rom_map_elem_t jpegio_jpegencoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&jpegio_jpegencoder_encode_obj) },
};
static MP_DEFINE_CONST_DICT(jpegio_jpegencoder_locals_dict, jpegio_jpegencoder_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    jpegio_jpegencoder_type,
    MP_QSTR_JpegEncoder,
    MP_TYPE_FLAG_NONE,
    make_new, jpegio_jpegencoder_make_new,
    attr, jpegio_jpegencoder_attr,
    locals_dict, &jpegio_jpegencoder_locals_dict
    );
