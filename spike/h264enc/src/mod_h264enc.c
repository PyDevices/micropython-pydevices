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
#include "driver/ppa.h"
#endif

// Before chip revision 3 the encoder's DMA takes only packed YUV420
// (odd lines U Y Y, even lines V Y Y). The pixel processing accelerator
// converts the panel's RGB565 into that layout in hardware, so an RGB565
// input goes through a PPA scale-rotate-mirror pass first.

typedef struct _h264enc_obj_t {
    mp_obj_base_t base;
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    ppa_client_handle_t ppa;
    uint8_t *yuv;
    uint32_t yuv_len;
    uint32_t src_len;      // bytes of RGB565 (or YUV420) the caller must supply
    uint32_t ppa_us;
    uint16_t width, height;      // the source picture (the panel)
    uint16_t cw, ch;             // the encoded canvas; the picture sits centred in it
    uint8_t fps, gop;
    bool convert;
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
    if (self->ppa) {
        ppa_unregister_client(self->ppa);
        self->ppa = NULL;
    }
    if (self->yuv) {
        esp_h264_free(self->yuv);
        self->yuv = NULL;
    }
}

static mp_obj_t h264enc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max, ARG_out_size, ARG_fmt, ARG_canvas_w, ARG_canvas_h };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fps, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_gop, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_bitrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2000000} },
        { MP_QSTR_qp_min, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10} },
        { MP_QSTR_qp_max, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 45} },
        { MP_QSTR_out_size, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fmt, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_ROM_QSTR(MP_QSTR_rgb565)} },
        { MP_QSTR_canvas_w, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_canvas_h, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);
    int w = args[ARG_width].u_int, h = args[ARG_height].u_int;
    if (w < 80 || w > 1920 || h < 80 || h > 2032 || (w & 15) || (h & 15)) {
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be multiples of 16, 80..1920 x 80..2032"));
    }
    int cw = args[ARG_canvas_w].u_int ? args[ARG_canvas_w].u_int : w;
    int ch = args[ARG_canvas_h].u_int ? args[ARG_canvas_h].u_int : h;
    if (cw < w || ch < h || (cw & 15) || (ch & 15) || cw > 1920 || ch > 2032) {
        mp_raise_ValueError(MP_ERROR_TEXT("canvas must be a multiple of 16 and at least the picture"));
    }
    h264enc_obj_t *self = mp_obj_malloc_with_finaliser(h264enc_obj_t, type);
    self->enc = NULL;
    self->out_buf = NULL;
    self->ppa = NULL;
    self->yuv = NULL;
    self->open = false;
    self->ppa_us = 0;
    qstr fmt = mp_obj_str_get_qstr(args[ARG_fmt].u_obj);
    if (fmt == MP_QSTR_rgb565) {
        self->convert = true;
        self->src_len = (uint32_t)w * h * 2;
    } else if (fmt == MP_QSTR_yuv420) {
        if (cw != w || ch != h) {
            mp_raise_ValueError(MP_ERROR_TEXT("a canvas needs an rgb565 source"));
        }
        self->convert = false;
        self->src_len = (uint32_t)w * h * 3 / 2;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("fmt must be 'rgb565' or 'yuv420'"));
    }
    self->width = w;
    self->height = h;
    self->cw = cw;
    self->ch = ch;
    self->fps = args[ARG_fps].u_int;
    self->gop = args[ARG_gop].u_int;
    self->in_len = (uint32_t)cw * ch * 3 / 2;   // what the encoder reads: the packed YUV420 canvas
    self->frames = 0;
    self->last_len = 0;
    self->last_type = -1;
    self->last_us = 0;

    if (self->convert) {
        ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
        if (ppa_register_client(&pcfg, &self->ppa) != ESP_OK) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("h264enc: PPA client"));
        }
        uint32_t want_yuv = ((uint32_t)cw * ch * 3 / 2 + 127) & ~127u;
        self->yuv = esp_h264_aligned_calloc(128, 1, want_yuv, &self->yuv_len, ESP_H264_MEM_SPIRAM);
        if (!self->yuv) {
            h264enc_close_int(self);
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("h264enc: YUV buffer"));
        }
        // black borders once: every packed line is (U|V, Y, Y) triplets, so
        // 0x80 0x10 0x10 repeated is limited-range black on both line kinds
        for (uint32_t i = 0; i + 2 < self->in_len; i += 3) {
            self->yuv[i] = 0x80;
            self->yuv[i + 1] = 0x10;
            self->yuv[i + 2] = 0x10;
        }
    }
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    cfg.gop = self->gop;
    cfg.fps = self->fps;
    cfg.res.width = cw;
    cfg.res.height = ch;
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
    if (in.len < self->src_len) {
        mp_raise_ValueError(MP_ERROR_TEXT("input buffer too small for the picture"));
    }
    esp_h264_enc_in_frame_t inf = {0};
    if (self->convert) {
        int64_t p0 = esp_timer_get_time();
        ppa_srm_oper_config_t op = {0};
        op.in.buffer = in.buf;
        op.in.pic_w = self->width;
        op.in.pic_h = self->height;
        op.in.block_w = self->width;
        op.in.block_h = self->height;
        op.in.block_offset_x = 0;
        op.in.block_offset_y = 0;
        op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
        op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        op.out.buffer = self->yuv;
        op.out.buffer_size = self->yuv_len;
        op.out.pic_w = self->cw;
        op.out.pic_h = self->ch;
        op.out.block_offset_x = ((self->cw - self->width) / 2) & ~1u;
        op.out.block_offset_y = ((self->ch - self->height) / 2) & ~1u;
        op.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
        op.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
        op.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
        op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        op.scale_x = 1.0f;
        op.scale_y = 1.0f;
        op.rgb_swap = false;
        op.byte_swap = false;
        op.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
        op.mode = PPA_TRANS_MODE_BLOCKING;
        esp_err_t perr = ppa_do_scale_rotate_mirror(self->ppa, &op);
        self->ppa_us = (uint32_t)(esp_timer_get_time() - p0);
        if (perr != ESP_OK) {
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("PPA convert failed: %d"), (int)perr);
        }
        inf.raw_data.buffer = self->yuv;
    } else {
        inf.raw_data.buffer = in.buf;
    }
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

// The converted picture (packed YUV420) of the last encode(), for inspection.
static mp_obj_t h264enc_yuv(mp_obj_t self_in) {
    h264enc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->yuv) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(self->yuv, self->in_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_yuv_obj, h264enc_yuv);

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
            case MP_QSTR_ppa_us: dest[0] = mp_obj_new_int(self->ppa_us); return;
            case MP_QSTR_length: dest[0] = mp_obj_new_int(self->last_len); return;
            case MP_QSTR_frames: dest[0] = mp_obj_new_int(self->frames); return;
            case MP_QSTR_width: dest[0] = mp_obj_new_int(self->width); return;
            case MP_QSTR_height: dest[0] = mp_obj_new_int(self->height); return;
            case MP_QSTR_canvas_w: dest[0] = mp_obj_new_int(self->cw); return;
            case MP_QSTR_canvas_h: dest[0] = mp_obj_new_int(self->ch); return;
            default: break;
        }
        dest[1] = MP_OBJ_SENTINEL;  // fall back to the locals dict
    }
}

static const mp_rom_map_elem_t h264enc_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&h264enc_encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_last), MP_ROM_PTR(&h264enc_last_obj) },
    { MP_ROM_QSTR(MP_QSTR_yuv), MP_ROM_PTR(&h264enc_yuv_obj) },
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
