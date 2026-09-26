// h264enc (spike): the ESP32-P4 hardware H.264 encoder as a MicroPython module.
//
//   enc = h264enc.Encoder(width, height, fps=30, gop=30, bitrate=2_000_000)
//   n = enc.encode(rgb565_buffer, out_bytearray)   # Annex-B bytes written into out
//   enc.frame_type, enc.us                           # of the last frame
//
// The input is RGB565 little-endian, width*height*2 bytes, in PSRAM or internal
// RAM; the mipidsi Display object's buffer protocol hands over the panel's own
// framebuffer, so a cast reads what the panel shows with no copy.
#include <string.h>
#include "py/runtime.h"
#include "py/obj.h"
#include "py/mperrno.h"

// Built only for the ESP32-P4 (the CAST_P4 board manifest names this module);
// the qstr pass must see every MP_QSTR_ name, so nothing here is guarded.
// The qstr pass only preprocesses (with NO_QSTR set) and does not see the
// component's include paths, so the IDF and esp_h264 headers stay out of it.
#ifndef NO_QSTR
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#endif

typedef struct _h264enc_obj_t {
    mp_obj_base_t base;
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    uint16_t width, height;
    uint8_t fps, gop;
    uint32_t in_len;
    uint8_t *out_buf;
    uint32_t out_cap;
    uint32_t frames;
    uint32_t last_us;
    uint32_t last_len;
    int last_type;
    bool open;
} h264enc_obj_t;

static void h264enc_check(esp_h264_err_t err, const char *what) {
    if (err != ESP_H264_ERR_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s: esp_h264 error %d"), what, (int)err);
    }
}

static void h264enc_close_int(h264enc_obj_t *self) {
    if (self->open) {
        esp_h264_enc_close(self->enc);
        self->open = false;
    }
    if (self->enc) {
        esp_h264_enc_del(self->enc);
        self->enc = NULL;
    }
    if (self->out_buf) {
        esp_h264_free(self->out_buf);
        self->out_buf = NULL;
    }
}

static mp_obj_t h264enc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max, ARG_out_size };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fps, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_gop, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_bitrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2000000} },
        { MP_QSTR_qp_min, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10} },
        { MP_QSTR_qp_max, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 45} },
        { MP_QSTR_out_size, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int w = args[ARG_width].u_int, h = args[ARG_height].u_int;
    if (w < 80 || w > 1920 || h < 80 || h > 2032 || (w & 15) || (h & 15)) {
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be multiples of 16, 80..1920 x 80..2032"));
    }
    h264enc_obj_t *self = mp_obj_malloc_with_finaliser(h264enc_obj_t, type);
    self->enc = NULL;
    self->out_buf = NULL;
    self->open = false;
    self->width = w;
    self->height = h;
    self->fps = args[ARG_fps].u_int;
    self->gop = args[ARG_gop].u_int;
    self->in_len = (uint32_t)w * h * 2;
    self->frames = 0;
    self->last_len = 0;
    self->last_type = -1;
    self->last_us = 0;

    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.pic_type = ESP_H264_RAW_FMT_RGB565_LE;
    cfg.gop = self->gop;
    cfg.fps = self->fps;
    cfg.res.width = w;
    cfg.res.height = h;
    cfg.rc.bitrate = args[ARG_bitrate].u_int;
    cfg.rc.qp_min = args[ARG_qp_min].u_int;
    cfg.rc.qp_max = args[ARG_qp_max].u_int;
    h264enc_check(esp_h264_enc_hw_new(&cfg, &self->enc), "esp_h264_enc_hw_new");
    esp_h264_err_t err = esp_h264_enc_open(self->enc);
    if (err != ESP_H264_ERR_OK) {
        h264enc_close_int(self);
        h264enc_check(err, "esp_h264_enc_open");
    }
    self->open = true;
    err = esp_h264_enc_hw_get_param_hd(self->enc, &self->param);
    if (err != ESP_H264_ERR_OK) {
        h264enc_close_int(self);
        h264enc_check(err, "esp_h264_enc_hw_get_param_hd");
    }
    uint32_t want = args[ARG_out_size].u_int ? (uint32_t)args[ARG_out_size].u_int : (uint32_t)w * h;
    self->out_buf = esp_h264_aligned_calloc(16, 1, want, &self->out_cap, ESP_H264_MEM_SPIRAM);
    if (!self->out_buf) {
        h264enc_close_int(self);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("h264enc: output buffer"));
    }
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t h264enc_encode(size_t n_args, const mp_obj_t *args) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->open) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("encoder is closed"));
    }
    mp_buffer_info_t in;
    mp_get_buffer_raise(args[1], &in, MP_BUFFER_READ);
    if (in.len < self->in_len) {
        mp_raise_ValueError(MP_ERROR_TEXT("input buffer smaller than width*height*2"));
    }
    esp_h264_enc_in_frame_t inf = {0};
    inf.raw_data.buffer = in.buf;
    inf.raw_data.len = self->in_len;
    inf.pts = (uint32_t)((uint64_t)self->frames * 1000 / self->fps);
    esp_h264_enc_out_frame_t outf = {0};
    outf.raw_data.buffer = self->out_buf;
    outf.raw_data.len = self->out_cap;
    int64_t t0 = esp_timer_get_time();
    esp_h264_err_t err = esp_h264_enc_process(self->enc, &inf, &outf);
    self->last_us = (uint32_t)(esp_timer_get_time() - t0);
    h264enc_check(err, "esp_h264_enc_process");
    self->last_len = outf.length;
    self->last_type = outf.frame_type;
    self->frames++;
    if (n_args > 2 && args[2] != mp_const_none) {
        mp_buffer_info_t out;
        mp_get_buffer_raise(args[2], &out, MP_BUFFER_WRITE);
        if (out.len < outf.length) {
            mp_raise_ValueError(MP_ERROR_TEXT("output bytearray too small"));
        }
        memcpy(out.buf, self->out_buf, outf.length);
    }
    return mp_obj_new_int(outf.length);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(h264enc_encode_obj, 2, 3, h264enc_encode);

// A view of the last frame's bytes without a copy (valid until the next encode()).
static mp_obj_t h264enc_last(mp_obj_t self_in) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bytes(self->out_buf, self->last_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_last_obj, h264enc_last);

static mp_obj_t h264enc_force_idr(mp_obj_t self_in) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_check(esp_h264_enc_force_idr((esp_h264_enc_param_handle_t)self->param), "force_idr");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_force_idr_obj, h264enc_force_idr);

static mp_obj_t h264enc_set_bitrate(mp_obj_t self_in, mp_obj_t bps) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_check(esp_h264_enc_set_bitrate((esp_h264_enc_param_handle_t)self->param, mp_obj_get_int(bps)), "set_bitrate");
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(h264enc_set_bitrate_obj, h264enc_set_bitrate);

static mp_obj_t h264enc_close(mp_obj_t self_in) {
    h264enc_close_int(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_close_obj, h264enc_close);

static void h264enc_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        switch (attr) {
            case MP_QSTR_frame_type: dest[0] = mp_obj_new_int(self->last_type); return;
            case MP_QSTR_us: dest[0] = mp_obj_new_int(self->last_us); return;
            case MP_QSTR_length: dest[0] = mp_obj_new_int(self->last_len); return;
            case MP_QSTR_frames: dest[0] = mp_obj_new_int(self->frames); return;
            case MP_QSTR_width: dest[0] = mp_obj_new_int(self->width); return;
            case MP_QSTR_height: dest[0] = mp_obj_new_int(self->height); return;
            default: break;
        }
        dest[1] = MP_OBJ_SENTINEL;  // fall back to the locals dict
    }
}

static const mp_rom_map_elem_t h264enc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&h264enc_encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_last), MP_ROM_PTR(&h264enc_last_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_idr), MP_ROM_PTR(&h264enc_force_idr_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_bitrate), MP_ROM_PTR(&h264enc_set_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&h264enc_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&h264enc_close_obj) },
};
static MP_DEFINE_CONST_DICT(h264enc_locals_dict, h264enc_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    h264enc_encoder_type,
    MP_QSTR_Encoder,
    MP_TYPE_FLAG_NONE,
    make_new, h264enc_make_new,
    attr, h264enc_attr,
    locals_dict, &h264enc_locals_dict
    );
static mp_obj_t h264enc_available(void) {
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(h264enc_available_obj, h264enc_available);

static const mp_rom_map_elem_t h264enc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_h264enc) },
    { MP_ROM_QSTR(MP_QSTR_available), MP_ROM_PTR(&h264enc_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_Encoder), MP_ROM_PTR(&h264enc_encoder_type) },
    { MP_ROM_QSTR(MP_QSTR_IDR), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_I), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_P), MP_ROM_INT(2) },
};
static MP_DEFINE_CONST_DICT(h264enc_module_globals, h264enc_module_globals_table);

const mp_obj_module_t h264enc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&h264enc_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_h264enc, h264enc_user_cmodule);
