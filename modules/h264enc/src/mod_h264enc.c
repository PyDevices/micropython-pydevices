// h264enc: RGB565 frames to H.264 on the ESP32-P4's hardware encoder.
//
//   enc = h264enc.Encoder(720, 720, fps=30, gop=30, bitrate=3_000_000)
//   au = enc.encode(framebuffer)        # bytes: Annex B NAL units
//   enc.keyframe; enc.force_idr(); enc.set_bitrate(n); enc.stats(); enc.close()
//
// The PPA converts each frame to the packed YUV420 the encoder reads (the only
// raw format the pre-revision-3 P4 takes), placed on a canvas that may be
// larger than the frame; esp_h264 encodes it. Limited-range BT.601, which is
// what an H.264 decoder assumes when the stream says nothing.
//
// The same session is available to C (h264enc.h): castif's cast task encodes
// through it on core 0. The P4 has one encoder, so one session at a time; a
// second open gets "busy" and the first carries on.

#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"

// The QSTR pass only preprocesses, with this module's include paths, not
// IDF's: the IDF headers stay out of it.
#ifndef NO_QSTR
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "driver/ppa.h"
#endif

#include "h264enc.h"

#define H264ENC_AU_MAX (256 * 1024)

struct h264enc_session {
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    bool enc_open;
    ppa_client_handle_t ppa;
    uint8_t *yuv;
    uint32_t yuv_len;
    uint32_t in_len;            // the packed YUV420 canvas the encoder reads
    uint8_t *out;
    uint32_t out_cap;
    uint16_t w, h, cw, ch;
    uint32_t frames;
    uint32_t ppa_us, enc_us;
};

// The one session the hardware can hold.
static h264enc_session_t *h264enc_owner;

// --- C API -------------------------------------------------------------------

void h264enc_close(h264enc_session_t *s) {
    if (s == NULL) {
        return;
    }
    if (s->enc_open) {
        esp_h264_enc_close(s->enc);
    }
    if (s->enc) {
        esp_h264_enc_del(s->enc);
    }
    if (s->ppa) {
        ppa_unregister_client(s->ppa);
    }
    if (s->yuv) {
        esp_h264_free(s->yuv);
    }
    if (s->out) {
        esp_h264_free(s->out);
    }
    if (h264enc_owner == s) {
        h264enc_owner = NULL;
    }
    heap_caps_free(s);
}

const char *h264enc_open(h264enc_session_t **out, int width, int height, int canvas_w, int canvas_h,
    int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size) {
    *out = NULL;
    if (h264enc_owner != NULL) {
        return "busy";
    }
    if (width < 80 || width > 1920 || height < 80 || height > 2032 || (width & 15) || (height & 15)) {
        return "width and height are multiples of 16, 80..1920 x 80..2032";
    }
    int cw = canvas_w ? canvas_w : width;
    int ch = canvas_h ? canvas_h : height;
    if (cw < width || ch < height || (cw & 15) || (ch & 15) || cw > 1920 || ch > 2032) {
        return "the canvas is a multiple of 16 and at least the frame";
    }
    if (fps < 1 || fps > 255 || gop < 1 || gop > 255) {
        return "fps and gop are 1..255";
    }
    h264enc_session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s == NULL) {
        return "no memory";
    }
    s->w = width;
    s->h = height;
    s->cw = cw;
    s->ch = ch;
    s->in_len = (uint32_t)cw * ch * 3 / 2;

    const char *why = NULL;
    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pcfg, &s->ppa) != ESP_OK) {
        s->ppa = NULL;
        why = "no PPA client";
        goto fail;
    }
    s->yuv = esp_h264_aligned_calloc(128, 1, (s->in_len + 127) & ~127u, &s->yuv_len, ESP_H264_MEM_SPIRAM);
    if (s->yuv == NULL) {
        why = "no memory for the YUV canvas";
        goto fail;
    }
    // The canvas border stays limited-range black (packed U Y Y / V Y Y).
    for (uint32_t i = 0; i + 2 < s->in_len; i += 3) {
        s->yuv[i] = 0x80;
        s->yuv[i + 1] = 0x10;
        s->yuv[i + 2] = 0x10;
    }
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    cfg.gop = (uint8_t)gop;
    cfg.fps = (uint8_t)fps;
    cfg.res.width = cw;
    cfg.res.height = ch;
    cfg.rc.bitrate = bitrate;
    cfg.rc.qp_min = qp_min;
    cfg.rc.qp_max = qp_max;
    if (esp_h264_enc_hw_new(&cfg, &s->enc) != ESP_H264_ERR_OK) {
        s->enc = NULL;
        why = "the encoder refused these settings (qp_min <= qp_max <= 51?)";
        goto fail;
    }
    if (esp_h264_enc_open(s->enc) != ESP_H264_ERR_OK) {
        why = "the encoder would not open";
        goto fail;
    }
    s->enc_open = true;
    if (esp_h264_enc_hw_get_param_hd(s->enc, &s->param) != ESP_H264_ERR_OK) {
        why = "no encoder parameter handle";
        goto fail;
    }
    s->out = esp_h264_aligned_calloc(16, 1, out_size ? out_size : H264ENC_AU_MAX, &s->out_cap, ESP_H264_MEM_SPIRAM);
    if (s->out == NULL) {
        why = "no memory for the output buffer";
        goto fail;
    }
    h264enc_owner = s;
    *out = s;
    return NULL;

fail:
    h264enc_close(s);
    return why;
}

int h264enc_encode(h264enc_session_t *s, const uint8_t *rgb565, const uint8_t **data, uint32_t *len, bool *idr) {
    int64_t t0 = esp_timer_get_time();
    ppa_srm_oper_config_t op = {0};
    op.in.buffer = (void *)rgb565;
    op.in.pic_w = s->w;
    op.in.pic_h = s->h;
    op.in.block_w = s->w;
    op.in.block_h = s->h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = s->yuv;
    op.out.buffer_size = s->yuv_len;
    op.out.pic_w = s->cw;
    op.out.pic_h = s->ch;
    op.out.block_offset_x = ((s->cw - s->w) / 2) & ~1u;
    op.out.block_offset_y = ((s->ch - s->h) / 2) & ~1u;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    op.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t perr = ppa_do_scale_rotate_mirror(s->ppa, &op);
    int64_t t1 = esp_timer_get_time();
    s->ppa_us = (uint32_t)(t1 - t0);
    if (perr != ESP_OK) {
        return -1;
    }
    esp_h264_enc_in_frame_t inf = {0};
    inf.raw_data.buffer = s->yuv;
    inf.raw_data.len = s->in_len;
    inf.pts = s->frames;
    esp_h264_enc_out_frame_t outf = {0};
    outf.raw_data.buffer = s->out;
    outf.raw_data.len = s->out_cap;
    esp_h264_err_t err = esp_h264_enc_process(s->enc, &inf, &outf);
    s->enc_us = (uint32_t)(esp_timer_get_time() - t1);
    if (err != ESP_H264_ERR_OK) {
        return (int)err;
    }
    s->frames++;
    *data = s->out;
    *len = outf.length;
    *idr = outf.frame_type == ESP_H264_FRAME_TYPE_IDR;
    return 0;
}

void h264enc_force_idr(h264enc_session_t *s) {
    esp_h264_enc_force_idr((esp_h264_enc_param_handle_t)s->param);
}

void h264enc_set_bitrate(h264enc_session_t *s, uint32_t bps) {
    esp_h264_enc_set_bitrate((esp_h264_enc_param_handle_t)s->param, bps);
}

void h264enc_timing(h264enc_session_t *s, uint32_t *ppa_us, uint32_t *enc_us) {
    *ppa_us = s->ppa_us;
    *enc_us = s->enc_us;
}

// --- h264enc.Encoder -----------------------------------------------------------

typedef struct _h264enc_encoder_obj_t {
    mp_obj_base_t base;
    h264enc_session_t *s;
    bool keyframe;              // the last encode() was an IDR frame
    uint32_t frames, keyframes, errors;
    uint64_t bytes;
} h264enc_encoder_obj_t;

static h264enc_session_t *h264enc_get(h264enc_encoder_obj_t *self) {
    if (self->s == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("h264enc: the encoder is closed"));
    }
    return self->s;
}

static mp_obj_t h264enc_encoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max, ARG_canvas_w, ARG_canvas_h, ARG_out_size };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fps, MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_gop, MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_bitrate, MP_ARG_INT, {.u_int = 3000000} },
        { MP_QSTR_qp_min, MP_ARG_INT, {.u_int = 10} },
        { MP_QSTR_qp_max, MP_ARG_INT, {.u_int = 45} },
        { MP_QSTR_canvas_w, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_canvas_h, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_out_size, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    h264enc_session_t *s;
    const char *why = h264enc_open(&s, args[ARG_width].u_int, args[ARG_height].u_int,
        args[ARG_canvas_w].u_int, args[ARG_canvas_h].u_int, args[ARG_fps].u_int, args[ARG_gop].u_int,
        args[ARG_bitrate].u_int, args[ARG_qp_min].u_int, args[ARG_qp_max].u_int, (uint32_t)args[ARG_out_size].u_int);
    if (why != NULL) {
        if (strcmp(why, "busy") == 0) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: the H.264 encoder is busy (a cast or another Encoder has it)"));
        }
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("h264enc: %s"), why);
    }
    h264enc_encoder_obj_t *self = mp_obj_malloc_with_finaliser(h264enc_encoder_obj_t, type);
    self->s = s;
    self->keyframe = false;
    self->frames = self->keyframes = self->errors = 0;
    self->bytes = 0;
    return MP_OBJ_FROM_PTR(self);
}

// encode(frame) -> bytes: one access unit, Annex B.
static mp_obj_t h264enc_encoder_encode(mp_obj_t self_in, mp_obj_t frame) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_session_t *s = h264enc_get(self);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(frame, &buf, MP_BUFFER_READ);
    if (buf.len < (size_t)s->w * s->h * 2) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("h264enc: a %dx%d frame is %d bytes, got %d"),
            s->w, s->h, s->w * s->h * 2, (int)buf.len);
    }
    const uint8_t *data;
    uint32_t len;
    bool idr;
    int err = h264enc_encode(s, buf.buf, &data, &len, &idr);
    if (err != 0) {
        self->errors++;
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: encode failed (%d)"), err);
    }
    self->keyframe = idr;
    self->frames++;
    self->keyframes += idr;
    self->bytes += len;
    return mp_obj_new_bytes(data, len);
}
static MP_DEFINE_CONST_FUN_OBJ_2(h264enc_encoder_encode_obj, h264enc_encoder_encode);

static mp_obj_t h264enc_encoder_force_idr(mp_obj_t self_in) {
    h264enc_force_idr(h264enc_get(MP_OBJ_TO_PTR(self_in)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_force_idr_obj, h264enc_encoder_force_idr);

static mp_obj_t h264enc_encoder_set_bitrate(mp_obj_t self_in, mp_obj_t bps) {
    mp_int_t b = mp_obj_get_int(bps);
    if (b < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("h264enc: bitrate must be positive"));
    }
    h264enc_set_bitrate(h264enc_get(MP_OBJ_TO_PTR(self_in)), (uint32_t)b);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(h264enc_encoder_set_bitrate_obj, h264enc_encoder_set_bitrate);

static mp_obj_t h264enc_encoder_stats(mp_obj_t self_in) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_session_t *s = h264enc_get(self);
    mp_obj_t d = mp_obj_new_dict(0);
    #define PUT(k, v) mp_obj_dict_store(d, MP_ROM_QSTR(k), mp_obj_new_int_from_ull(v))
    PUT(MP_QSTR_frames, self->frames);
    PUT(MP_QSTR_keyframes, self->keyframes);
    PUT(MP_QSTR_bytes, self->bytes);
    PUT(MP_QSTR_errors, self->errors);
    PUT(MP_QSTR_ppa_us, s->ppa_us);
    PUT(MP_QSTR_enc_us, s->enc_us);
    #undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_stats_obj, h264enc_encoder_stats);

static mp_obj_t h264enc_encoder_close(mp_obj_t self_in) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_close(self->s);
    self->s = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_close_obj, h264enc_encoder_close);

static mp_obj_t h264enc_encoder_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return h264enc_encoder_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(h264enc_encoder_exit_obj, 4, 4, h264enc_encoder_exit);

static void h264enc_encoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_keyframe) {
        dest[0] = mp_obj_new_bool(self->keyframe);
    } else if (attr == MP_QSTR_width || attr == MP_QSTR_height) {
        h264enc_session_t *s = h264enc_get(self);
        dest[0] = MP_OBJ_NEW_SMALL_INT(attr == MP_QSTR_width ? s->w : s->h);
    } else {
        dest[1] = MP_OBJ_SENTINEL;
    }
}

static const mp_rom_map_elem_t h264enc_encoder_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_encode), MP_ROM_PTR(&h264enc_encoder_encode_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_idr), MP_ROM_PTR(&h264enc_encoder_force_idr_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_bitrate), MP_ROM_PTR(&h264enc_encoder_set_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&h264enc_encoder_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&h264enc_encoder_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&h264enc_encoder_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&h264enc_encoder_exit_obj) },
};
static MP_DEFINE_CONST_DICT(h264enc_encoder_locals_dict, h264enc_encoder_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    h264enc_encoder_type,
    MP_QSTR_Encoder,
    MP_TYPE_FLAG_NONE,
    make_new, h264enc_encoder_make_new,
    attr, h264enc_encoder_attr,
    locals_dict, &h264enc_encoder_locals_dict
    );

static const mp_rom_map_elem_t h264enc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_h264enc) },
    { MP_ROM_QSTR(MP_QSTR_Encoder), MP_ROM_PTR(&h264enc_encoder_type) },
};
static MP_DEFINE_CONST_DICT(h264enc_module_globals, h264enc_module_globals_table);

const mp_obj_module_t h264enc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&h264enc_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_h264enc, h264enc_user_cmodule);
