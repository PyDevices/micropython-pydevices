// jpegdec (spike): the ESP32-P4 hardware JPEG decoder as a MicroPython module.
//
//   dec = jpegdec.Decoder()
//   dec.decode(jpeg_bytes, out_rgb565_buffer)   # e.g. the panel's own framebuffer
//   dec.info, dec.us, dec.out_size              # of the last frame
//
// The input is copied into a DMA-capable buffer the driver allocates; the output
// buffer is the caller's and must be cache-line aligned (the mipidsi framebuffer is).
#include <string.h>
#include "py/runtime.h"
#include "py/obj.h"

#ifndef NO_QSTR
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#endif

typedef struct _jpegdec_obj_t {
    mp_obj_base_t base;
    jpeg_decoder_handle_t dec;
    uint8_t *in;
    size_t in_cap;
    jpeg_decode_picture_info_t info;
    uint32_t last_us;
    uint32_t out_size;
    bool have_info;
    bool bgr;
} jpegdec_obj_t;

static void jpegdec_check(esp_err_t err, const char *what) {
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s: esp error %d"), what, (int)err);
    }
}

static void jpegdec_close_int(jpegdec_obj_t *self) {
    if (self->dec) {
        jpeg_del_decoder_engine(self->dec);
        self->dec = NULL;
    }
    if (self->in) {
        heap_caps_free(self->in);
        self->in = NULL;
        self->in_cap = 0;
    }
}

static mp_obj_t jpegdec_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_timeout_ms, ARG_bgr };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 200} },
        { MP_QSTR_bgr, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    jpegdec_obj_t *self = mp_obj_malloc_with_finaliser(jpegdec_obj_t, type);
    self->dec = NULL;
    self->in = NULL;
    self->in_cap = 0;
    self->have_info = false;
    self->last_us = 0;
    self->out_size = 0;
    self->bgr = args[ARG_bgr].u_bool;
    jpeg_decode_engine_cfg_t cfg = { .intr_priority = 0, .timeout_ms = args[ARG_timeout_ms].u_int };
    jpegdec_check(jpeg_new_decoder_engine(&cfg, &self->dec), "jpeg_new_decoder_engine");
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t jpegdec_decode(mp_obj_t self_in, mp_obj_t data_in, mp_obj_t out_in) {
    jpegdec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->dec) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("decoder is closed"));
    }
    mp_buffer_info_t data, out;
    mp_get_buffer_raise(data_in, &data, MP_BUFFER_READ);
    mp_get_buffer_raise(out_in, &out, MP_BUFFER_WRITE);
    if (data.len > self->in_cap) {
        if (self->in) {
            heap_caps_free(self->in);
            self->in = NULL;
        }
        jpeg_decode_memory_alloc_cfg_t mcfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        size_t got = 0;
        self->in = jpeg_alloc_decoder_mem(data.len + 4096, &mcfg, &got);
        if (!self->in) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("jpegdec: input buffer"));
        }
        self->in_cap = got;
    }
    memcpy(self->in, data.buf, data.len);
    jpegdec_check(jpeg_decoder_get_info(self->in, data.len, &self->info), "jpeg_decoder_get_info");
    self->have_info = true;
    if ((size_t)self->info.width * self->info.height * 2 > out.len) {
        mp_raise_ValueError(MP_ERROR_TEXT("output buffer smaller than the picture"));
    }
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = self->bgr ? JPEG_DEC_RGB_ELEMENT_ORDER_BGR : JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_decoder_process(self->dec, &dcfg, self->in, data.len, out.buf, out.len, &out_size);
    self->last_us = (uint32_t)(esp_timer_get_time() - t0);
    jpegdec_check(err, "jpeg_decoder_process");
    self->out_size = out_size;
    return mp_obj_new_int(out_size);
}
static MP_DEFINE_CONST_FUN_OBJ_3(jpegdec_decode_obj, jpegdec_decode);

static mp_obj_t jpegdec_close(mp_obj_t self_in) {
    jpegdec_close_int(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(jpegdec_close_obj, jpegdec_close);

static void jpegdec_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    jpegdec_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        switch (attr) {
            case MP_QSTR_us: dest[0] = mp_obj_new_int(self->last_us); return;
            case MP_QSTR_out_size: dest[0] = mp_obj_new_int(self->out_size); return;
            case MP_QSTR_info: {
                if (!self->have_info) {
                    dest[0] = mp_const_none;
                    return;
                }
                mp_obj_t items[3] = { mp_obj_new_int(self->info.width), mp_obj_new_int(self->info.height), mp_obj_new_int(self->info.sample_method) };
                dest[0] = mp_obj_new_tuple(3, items);
                return;
            }
            default: break;
        }
        dest[1] = MP_OBJ_SENTINEL;
    }
}

static const mp_rom_map_elem_t jpegdec_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&jpegdec_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&jpegdec_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&jpegdec_close_obj) },
};
static MP_DEFINE_CONST_DICT(jpegdec_locals_dict, jpegdec_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    jpegdec_decoder_type,
    MP_QSTR_Decoder,
    MP_TYPE_FLAG_NONE,
    make_new, jpegdec_make_new,
    attr, jpegdec_attr,
    locals_dict, &jpegdec_locals_dict
    );

static const mp_rom_map_elem_t jpegdec_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_jpegdec) },
    { MP_ROM_QSTR(MP_QSTR_Decoder), MP_ROM_PTR(&jpegdec_decoder_type) },
};
static MP_DEFINE_CONST_DICT(jpegdec_module_globals, jpegdec_module_globals_table);

const mp_obj_module_t jpegdec_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&jpegdec_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_jpegdec, jpegdec_user_cmodule);
