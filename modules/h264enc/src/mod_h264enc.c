// SPDX-License-Identifier: MIT
// h264enc: RGB565 frames to H.264 on the ESP32-P4's hardware encoder.
//
//   enc = h264enc.Encoder(720, 720, fps=30, gop=30, bitrate=3_000_000)
//   au = enc.encode(framebuffer)        # bytes: Annex B NAL units
//   enc.keyframe; enc.force_idr(); enc.set_bitrate(n); enc.stats(); enc.close()
//
//   enc = h264enc.Encoder(1920, 1072, fps=25, pipeline=True)
//   au = enc.encode(frame)              # the previous frame's access unit (b"" at first)
//   au = enc.flush()                    # the last one
//
// ppa's C API (the PPA) converts each frame to the packed YUV420 the encoder reads (the only
// raw format the pre-revision-3 P4 takes), placed on a canvas that may be
// larger than the frame; esp_h264 encodes it. Limited-range BT.601, which is
// what an H.264 decoder assumes when the stream says nothing.
//
// The same session is available to C (h264enc.h): castif's cast task encodes
// through it on core 0. The P4 has one encoder, so one session at a time; a
// second open gets "busy" and the first carries on.
//
// With pipeline=True the session has two canvases and a worker task. encode()
// converts this frame on the PPA (on the caller's thread, so the frame is free
// again when encode() returns) while the worker encodes the previous one, then
// hands the worker this frame and returns the previous frame's access unit.
// The PPA and the encoder are separate engines, so a frame costs about the
// longer of the two rather than their sum.

#include <stdio.h>
#include <string.h>

#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/objstr.h"

// The QSTR pass only preprocesses, with this module's include paths, not
// IDF's: the IDF headers stay out of it.
#ifndef NO_QSTR
#include "esp_heap_caps.h"
#include "esp_task.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#endif

#include "h264enc.h"

// The colour conversion is ppa's (modules/ppa/src/ppa_mod.h): declared here,
// weak, so h264enc builds without ppa and h264enc_open() says it's missing.
typedef struct ppa_mod_client ppa_mod_client_t;
typedef struct {
    const void *src;
    uint32_t src_w, src_h;
    int src_fmt;
    uint32_t sx, sy, sw, sh;
    void *dst;
    size_t dst_len;
    uint32_t dst_w, dst_h;
    int dst_fmt;
    uint32_t x, y, w, h;
    int rotate;
    bool mirror_x, mirror_y;
    bool swap;
    bool yuv_limited;
    bool approx;
} ppa_mod_srm_t;
extern ppa_mod_client_t *ppa_mod_open(int hw) __attribute__((weak));
extern void ppa_mod_close(ppa_mod_client_t *c) __attribute__((weak));
extern const char *ppa_mod_srm(ppa_mod_client_t *c, const ppa_mod_srm_t *op) __attribute__((weak));
#define H264ENC_PPA_RGB565 0
#define H264ENC_PPA_YUV420 6

// The output buffer, when out_size doesn't say: room for a keyframe of the
// whole canvas at its raw size. A busy picture (a camera's, with its sensor
// noise) can make a keyframe bigger than any fixed guess, and one that
// doesn't fit stops the encoder until its 10 s timeout; then the next frame
// is a keyframe of the same picture and overflows too, so it never recovers.
// Flat screens come nowhere near this. It's PSRAM, and 1.4 MB at 1280x720.
#define H264ENC_AU_MIN (256 * 1024)

struct h264enc_session {
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    bool enc_open;
    ppa_mod_client_t *ppa;
    uint8_t *yuv;
    uint8_t *yuv2;              // the second canvas, pipeline only
    uint32_t yuv_len;
    bool pipeline;
    uint8_t *next;              // the canvas the next frame is converted into
    bool busy;                  // the worker has a frame
    bool ready;                 // out holds an access unit nobody has taken yet
    int job_err;                // the worker's result for the frame it finished
    uint32_t job_len;
    bool job_idr;
    uint32_t in_len;            // the packed YUV420 canvas the encoder reads
    uint8_t *out;
    uint32_t out_cap;
    uint16_t w, h, cw, ch;
    uint32_t frames;
    uint32_t ppa_us, enc_us;
};

// The one session the hardware can hold.
static h264enc_session_t *h264enc_owner;

// --- the pipeline's worker ---------------------------------------------------
//
// One task for the life of the firmware, made by the first pipelined open:
// there is one encoder, so there is never more than one session to serve, and
// a task that is never deleted can't be freed under a session that is closing.

static TaskHandle_t h264enc_worker_task;
static SemaphoreHandle_t h264enc_go, h264enc_done;
static h264enc_session_t *volatile h264enc_job;

static int h264enc_run(h264enc_session_t *s, uint8_t *yuv, uint32_t *len, bool *idr);

static void h264enc_worker(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(h264enc_go, portMAX_DELAY);
        h264enc_session_t *s = h264enc_job;
        s->job_err = h264enc_run(s, s->next == s->yuv ? s->yuv2 : s->yuv, &s->job_len, &s->job_idr);
        xSemaphoreGive(h264enc_done);
    }
}

static const char *h264enc_worker_start(void) {
    if (h264enc_worker_task != NULL) {
        return NULL;
    }
    if (h264enc_go == NULL) {
        h264enc_go = xSemaphoreCreateBinary();
        h264enc_done = xSemaphoreCreateBinary();
        if (h264enc_go == NULL || h264enc_done == NULL) {
            return "no memory for the pipeline's semaphores";
        }
    }
    // The encoder call blocks on its interrupt for nearly all of a frame; the
    // stack is for its rate control and its error log.
    if (xTaskCreatePinnedToCore(h264enc_worker, "h264enc", 8192, NULL, ESP_TASK_PRIO_MIN + 2,
        &h264enc_worker_task, tskNO_AFFINITY) != pdPASS) {
        h264enc_worker_task = NULL;
        return "no memory for the pipeline's task";
    }
    return NULL;
}

// Waits for the frame with the worker, if any; its access unit is then in out.
static void h264enc_wait(h264enc_session_t *s) {
    if (s->busy) {
        xSemaphoreTake(h264enc_done, portMAX_DELAY);
        s->busy = false;
        s->ready = true;
    }
}

// Hands the canvas just converted (s->next) to the worker and flips canvases.
static void h264enc_submit(h264enc_session_t *s) {
    s->next = s->next == s->yuv ? s->yuv2 : s->yuv;
    s->busy = true;
    h264enc_job = s;
    xSemaphoreGive(h264enc_go);
}

// --- C API -------------------------------------------------------------------

void h264enc_close(h264enc_session_t *s) {
    if (s == NULL) {
        return;
    }
    h264enc_wait(s);
    if (s->enc_open) {
        esp_h264_enc_close(s->enc);
    }
    if (s->enc) {
        esp_h264_enc_del(s->enc);
    }
    if (s->ppa) {
        ppa_mod_close(s->ppa);
    }
    if (s->yuv) {
        esp_h264_free(s->yuv);
    }
    if (s->yuv2) {
        esp_h264_free(s->yuv2);
    }
    if (s->out) {
        esp_h264_free(s->out);
    }
    if (h264enc_owner == s) {
        h264enc_owner = NULL;
    }
    heap_caps_free(s);
}

// A canvas whose border is limited-range black (packed U Y Y / V Y Y).
static uint8_t *h264enc_canvas(h264enc_session_t *s) {
    uint32_t got;
    uint8_t *p = esp_h264_aligned_calloc(128, 1, (s->in_len + 127) & ~127u, &got, ESP_H264_MEM_SPIRAM);
    if (p == NULL) {
        return NULL;
    }
    s->yuv_len = got;
    for (uint32_t i = 0; i + 2 < s->in_len; i += 3) {
        p[i] = 0x80;
        p[i + 1] = 0x10;
        p[i + 2] = 0x10;
    }
    return p;
}

static const char *h264enc_open_pipelined(h264enc_session_t **out, int width, int height, int canvas_w,
    int canvas_h, int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size, bool pipeline);

const char *h264enc_open(h264enc_session_t **out, int width, int height, int canvas_w, int canvas_h,
    int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size) {
    return h264enc_open_pipelined(out, width, height, canvas_w, canvas_h, fps, gop, bitrate, qp_min, qp_max,
        out_size, false);
}

static const char *h264enc_open_pipelined(h264enc_session_t **out, int width, int height, int canvas_w,
    int canvas_h, int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size, bool pipeline) {
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
    if (ppa_mod_open == NULL) {
        why = "needs the ppa module in this firmware (it converts each frame for the encoder)";
        goto fail;
    }
    s->ppa = ppa_mod_open(1);       // the PPA, never the software path: this is the hot loop
    if (s->ppa == NULL) {
        why = "no memory for a ppa client";
        goto fail;
    }
    s->yuv = h264enc_canvas(s);
    if (s->yuv == NULL) {
        why = "no memory for the YUV canvas";
        goto fail;
    }
    s->next = s->yuv;
    if (pipeline) {
        s->yuv2 = h264enc_canvas(s);
        if (s->yuv2 == NULL) {
            why = "no memory for the second YUV canvas";
            goto fail;
        }
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
    esp_h264_err_t herr = esp_h264_enc_hw_new(&cfg, &s->enc);
    if (herr != ESP_H264_ERR_OK) {
        s->enc = NULL;
        // Its reference rows need one block of internal RAM, 1,152 bytes per
        // 16 pixels of width (92 KB at 1280); the canvases and the output
        // buffer come from PSRAM. Either pool can be short, free in total but
        // in pieces, so say which one and how big its largest piece is.
        if (herr == ESP_H264_ERR_MEM) {
            static char msg[112];
            const size_t need = (size_t)((cw + 15) / 16) * 1152;
            const size_t internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (internal < need) {
                snprintf(msg, sizeof(msg), "not enough internal RAM in one piece for the encoder (needs %u bytes, largest free piece %u)",
                    (unsigned)need, (unsigned)internal);
            } else {
                snprintf(msg, sizeof(msg), "not enough PSRAM in one piece for the encoder (largest free piece %u bytes)",
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            }
            why = msg;
        } else {
            why = "the encoder refused these settings (qp_min <= qp_max <= 51?)";
        }
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
    if (out_size == 0) {
        out_size = (uint32_t)cw * ch * 3 / 2 + 4096;
        if (out_size < H264ENC_AU_MIN) {
            out_size = H264ENC_AU_MIN;
        }
    }
    s->out = esp_h264_aligned_calloc(16, 1, out_size, &s->out_cap, ESP_H264_MEM_SPIRAM);
    if (s->out == NULL) {
        why = "no memory for the output buffer";
        goto fail;
    }
    // After the encoder, whose reference rows want the largest piece of
    // internal RAM: the task's stack comes from what is left.
    if (pipeline) {
        why = h264enc_worker_start();
        if (why != NULL) {
            goto fail;
        }
        s->pipeline = true;
    }
    h264enc_owner = s;
    *out = s;
    return NULL;

fail:
    h264enc_close(s);
    return why;
}

// RGB565 to the packed YUV420 the encoder reads, placed centred on the
// canvas; studio range, which is what H.264 decoders assume.
static int h264enc_convert(h264enc_session_t *s, const uint8_t *rgb565, uint8_t *yuv) {
    int64_t t0 = esp_timer_get_time();
    ppa_mod_srm_t op = {
        .src = rgb565, .src_w = s->w, .src_h = s->h, .src_fmt = H264ENC_PPA_RGB565,
        .dst = yuv, .dst_len = s->yuv_len, .dst_w = s->cw, .dst_h = s->ch, .dst_fmt = H264ENC_PPA_YUV420,
        .x = ((s->cw - s->w) / 2) & ~1u, .y = ((s->ch - s->h) / 2) & ~1u, .w = s->w, .h = s->h,
        .yuv_limited = true,
    };
    const char *perr = ppa_mod_srm(s->ppa, &op);
    s->ppa_us = (uint32_t)(esp_timer_get_time() - t0);
    return perr != NULL ? -1 : 0;
}

// One canvas through the encoder into out.
static int h264enc_run(h264enc_session_t *s, uint8_t *yuv, uint32_t *len, bool *idr) {
    int64_t t0 = esp_timer_get_time();
    esp_h264_enc_in_frame_t inf = {0};
    inf.raw_data.buffer = yuv;
    inf.raw_data.len = s->in_len;
    inf.pts = s->frames;
    esp_h264_enc_out_frame_t outf = {0};
    outf.raw_data.buffer = s->out;
    outf.raw_data.len = s->out_cap;
    esp_h264_err_t err = esp_h264_enc_process(s->enc, &inf, &outf);
    s->enc_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err != ESP_H264_ERR_OK) {
        return (int)err;
    }
    s->frames++;
    *len = outf.length;
    *idr = outf.frame_type == ESP_H264_FRAME_TYPE_IDR;
    return 0;
}

int h264enc_encode(h264enc_session_t *s, const uint8_t *rgb565, const uint8_t **data, uint32_t *len, bool *idr) {
    if (s->pipeline) {
        return -2;
    }
    int err = h264enc_convert(s, rgb565, s->yuv);
    if (err != 0) {
        return err;
    }
    err = h264enc_run(s, s->yuv, len, idr);
    if (err != 0) {
        return err;
    }
    *data = s->out;
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
    uint32_t wall_us;           // how long the last encode() kept its caller
    uint64_t bytes;
} h264enc_encoder_obj_t;

static h264enc_session_t *h264enc_get(h264enc_encoder_obj_t *self) {
    if (self->s == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("h264enc: the encoder is closed"));
    }
    return self->s;
}

static mp_obj_t h264enc_encoder_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max, ARG_canvas_w, ARG_canvas_h, ARG_out_size,
           ARG_pipeline };
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
        { MP_QSTR_pipeline, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    h264enc_session_t *s;
    const char *why = h264enc_open_pipelined(&s, args[ARG_width].u_int, args[ARG_height].u_int,
        args[ARG_canvas_w].u_int, args[ARG_canvas_h].u_int, args[ARG_fps].u_int, args[ARG_gop].u_int,
        args[ARG_bitrate].u_int, args[ARG_qp_min].u_int, args[ARG_qp_max].u_int, (uint32_t)args[ARG_out_size].u_int,
        args[ARG_pipeline].u_bool);
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
    self->wall_us = 0;
    self->bytes = 0;
    return MP_OBJ_FROM_PTR(self);
}

// Counts an access unit handed to Python, and makes its bytes.
static mp_obj_t h264enc_encoder_take(h264enc_encoder_obj_t *self, const uint8_t *data, uint32_t len, bool idr) {
    self->keyframe = idr;
    self->frames++;
    self->keyframes += idr;
    self->bytes += len;
    return mp_obj_new_bytes(data, len);
}

// The pipeline's finished access unit, or b"" when there is none; raises if
// the encoder failed on it.
static mp_obj_t h264enc_encoder_collect(h264enc_encoder_obj_t *self, h264enc_session_t *s) {
    if (!s->ready) {
        return mp_const_empty_bytes;
    }
    s->ready = false;
    if (s->job_err != 0) {
        self->errors++;
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: encode failed (%d)"), s->job_err);
    }
    return h264enc_encoder_take(self, s->out, s->job_len, s->job_idr);
}

// encode(frame) -> bytes: one access unit, Annex B. Pipelined, the previous
// frame's (b"" for the first frame).
static mp_obj_t h264enc_encoder_encode(mp_obj_t self_in, mp_obj_t frame) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_session_t *s = h264enc_get(self);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(frame, &buf, MP_BUFFER_READ);
    if (buf.len < (size_t)s->w * s->h * 2) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("h264enc: a %dx%d frame is %d bytes, got %d"),
            s->w, s->h, s->w * s->h * 2, (int)buf.len);
    }
    int64_t t0 = esp_timer_get_time();
    if (s->pipeline) {
        // Convert this frame while the worker encodes the last one, then wait
        // for it. The GIL is out for both, as below.
        MP_THREAD_GIL_EXIT();
        int perr = h264enc_convert(s, buf.buf, s->next);
        h264enc_wait(s);
        MP_THREAD_GIL_ENTER();
        if (perr != 0) {
            // Nothing goes to the worker, so frames still come out one for one:
            // the access unit waiting now comes out of the next encode().
            self->wall_us = (uint32_t)(esp_timer_get_time() - t0);
            self->errors++;
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: the PPA could not convert the frame"));
        }
        // The access unit is copied out before the worker may write the next.
        mp_obj_t au = mp_const_empty_bytes;
        int jerr = 0;
        if (s->ready) {
            s->ready = false;
            jerr = s->job_err;
            if (jerr == 0) {
                au = h264enc_encoder_take(self, s->out, s->job_len, s->job_idr);
            } else {
                self->errors++;
            }
        }
        h264enc_submit(s);
        self->wall_us = (uint32_t)(esp_timer_get_time() - t0);
        if (jerr != 0) {
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: encode failed (%d)"), jerr);
        }
        return au;
    }
    const uint8_t *data;
    uint32_t len;
    bool idr;
    // The PPA and the encoder are hardware waits: let other threads run, so
    // an encoder on a background thread (an HLS display, say) doesn't stall
    // the app drawing in the foreground. The frame is read as it stands,
    // which is how castif reads a panel too.
    MP_THREAD_GIL_EXIT();
    int err = h264enc_encode(s, buf.buf, &data, &len, &idr);
    MP_THREAD_GIL_ENTER();
    self->wall_us = (uint32_t)(esp_timer_get_time() - t0);
    if (err != 0) {
        self->errors++;
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("h264enc: encode failed (%d)"), err);
    }
    return h264enc_encoder_take(self, data, len, idr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(h264enc_encoder_encode_obj, h264enc_encoder_encode);

// flush() -> bytes: the frame still in the pipeline, or b"".
static mp_obj_t h264enc_encoder_flush(mp_obj_t self_in) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    h264enc_session_t *s = h264enc_get(self);
    MP_THREAD_GIL_EXIT();
    h264enc_wait(s);
    MP_THREAD_GIL_ENTER();
    return h264enc_encoder_collect(self, s);
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_flush_obj, h264enc_encoder_flush);

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
    PUT(MP_QSTR_wall_us, self->wall_us);
    #undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_stats_obj, h264enc_encoder_stats);

static void h264enc_encoder_release(h264enc_encoder_obj_t *self) {
    h264enc_close(self->s);
    self->s = NULL;
}

// close() -> bytes or None: pipelined, the last frame's access unit (b"" when
// there is none), as flush() would have returned it.
static mp_obj_t h264enc_encoder_close(mp_obj_t self_in) {
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->s == NULL) {
        return mp_const_none;
    }
    mp_obj_t last = mp_const_none;
    if (self->s->pipeline) {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            last = h264enc_encoder_flush(self_in);
            nlr_pop();
        } else {
            h264enc_encoder_release(self);
            nlr_jump(nlr.ret_val);
        }
    }
    h264enc_encoder_release(self);
    return last;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_close_obj, h264enc_encoder_close);

// The finaliser and __exit__ give the encoder back without allocating: the
// collector can't make bytes, and __exit__ must not return a true value.
static mp_obj_t h264enc_encoder_del(mp_obj_t self_in) {
    h264enc_encoder_release(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(h264enc_encoder_del_obj, h264enc_encoder_del);

static mp_obj_t h264enc_encoder_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return h264enc_encoder_del(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(h264enc_encoder_exit_obj, 4, 4, h264enc_encoder_exit);

static void h264enc_encoder_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    h264enc_encoder_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_keyframe) {
        dest[0] = mp_obj_new_bool(self->keyframe);
    } else if (attr == MP_QSTR_pipeline) {
        dest[0] = mp_obj_new_bool(h264enc_get(self)->pipeline);
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
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&h264enc_encoder_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&h264enc_encoder_del_obj) },
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

// h264enc.__revision__: which h264enc this firmware was built from. The build
// passes H264ENC_REVISION (git describe) as a compile definition; a build with no
// git says "unknown" rather than guessing.
#ifndef H264ENC_REVISION
#define H264ENC_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(h264enc_revision_obj, H264ENC_REVISION);

static const mp_rom_map_elem_t h264enc_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_h264enc) },
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&h264enc_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_Encoder), MP_ROM_PTR(&h264enc_encoder_type) },
};
static MP_DEFINE_CONST_DICT(h264enc_module_globals, h264enc_module_globals_table);

const mp_obj_module_t h264enc_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&h264enc_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_h264enc, h264enc_user_cmodule);
