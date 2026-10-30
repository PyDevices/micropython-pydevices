/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileCopyrightText: 2026 Brad Barnett
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// h264's encoder, in place of esp-vision's platform/h264.c: the same API
// (h264.h) and the same code, plus the pre-revision-3 ESP32-P4.
//
// esp-vision hands the encoder RGB565 straight from the image. The encoder
// in a P4 before revision 3 reads only packed YUV420 (ESP_H264_RAW_FMT_O_UYY_E_VYY),
// and esp_h264 refuses anything else with ESP_H264_ERR_ARG, so a firmware
// built for those chips (CONFIG_ESP_REV_MIN_FULL < 300) converts each frame
// here first. Revision 3 and later take RGB565 as esp-vision does.

#include "h264.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#ifndef CMSIS_MCU_H
#define CMSIS_MCU_H "cmsis_compiler.h"
#endif

#ifndef NO_QSTR
#include "imlib.h"
#endif

#include "esp_h264_alloc.h"
#include "esp_h264_enc_single_hw.h"

#if !defined(CONFIG_ESP_REV_MIN_FULL) || CONFIG_ESP_REV_MIN_FULL < 300
#define VISION_H264_YUV420 1
#else
#define VISION_H264_YUV420 0
#endif

struct esp_vision_h264_enc {
    esp_h264_enc_handle_t enc;
    int w;
    int h;
    uint8_t *in_buf;        // DMA-capable encoder input: RGB565, or packed YUV420
    uint32_t in_buf_size;   // actual allocation size (cache-line aligned)
    size_t raw_size;        // valid input bytes per frame
    uint8_t *out_buf;       // encoded NAL output
    uint32_t out_buf_size;  // actual allocation size
    uint32_t pts;           // monotonically increasing presentation timestamp
};

static esp_err_t esp_vision_h264_err_to_esp_err(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK:
        return ESP_OK;
    case ESP_H264_ERR_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_H264_ERR_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_H264_ERR_UNSUPPORTED:
        return ESP_ERR_NOT_SUPPORTED;
    case ESP_H264_ERR_TIMEOUT:
        return ESP_ERR_TIMEOUT;
    case ESP_H264_ERR_OVERFLOW:
        return ESP_ERR_INVALID_SIZE;
    case ESP_H264_ERR_FAIL:
    default:
        return ESP_FAIL;
    }
}

#if VISION_H264_YUV420

// RGB565 (little-endian, as the image holds it) to the encoder's packed
// YUV420: each pair of rows becomes "U Y Y" per two pixels on the first row
// and "V Y Y" on the second, 1.5 bytes a pixel. Limited-range BT.601, which
// is what an H.264 decoder assumes when the stream says nothing; each 2x2
// block shares the chroma of its average colour.
static void vision_h264_rgb565_to_uyy_vyy(const uint16_t *src, int w, int h, uint8_t *dst)
{
    const int out_stride = w * 3 / 2;
    for (int y = 0; y < h; y += 2) {
        const uint16_t *r0 = src + (size_t)y * w;
        const uint16_t *r1 = r0 + w;
        uint8_t *d0 = dst + (size_t)y * out_stride;
        uint8_t *d1 = d0 + out_stride;
        for (int x = 0; x < w; x += 2) {
            int rr[4], gg[4], bb[4];
            const uint16_t p[4] = { r0[x], r0[x + 1], r1[x], r1[x + 1] };
            int ys[4];
            int rs = 0, gs = 0, bs = 0;
            for (int i = 0; i < 4; i++) {
                rr[i] = (((p[i] >> 11) & 0x1f) * 527 + 23) >> 6;     // 5 bits to 8
                gg[i] = (((p[i] >> 5) & 0x3f) * 259 + 33) >> 6;     // 6 bits to 8
                bb[i] = ((p[i] & 0x1f) * 527 + 23) >> 6;
                ys[i] = ((66 * rr[i] + 129 * gg[i] + 25 * bb[i] + 128) >> 8) + 16;
                rs += rr[i];
                gs += gg[i];
                bs += bb[i];
            }
            rs = (rs + 2) >> 2;
            gs = (gs + 2) >> 2;
            bs = (bs + 2) >> 2;
            int u = ((-38 * rs - 74 * gs + 112 * bs + 128) >> 8) + 128;
            int v = ((112 * rs - 94 * gs - 18 * bs + 128) >> 8) + 128;
            d0[0] = (uint8_t)u;
            d0[1] = (uint8_t)ys[0];
            d0[2] = (uint8_t)ys[1];
            d1[0] = (uint8_t)v;
            d1[1] = (uint8_t)ys[2];
            d1[2] = (uint8_t)ys[3];
            d0 += 3;
            d1 += 3;
        }
    }
}

#endif

void esp_vision_h264_close(esp_vision_h264_enc_t *enc)
{
    if (enc == NULL) {
        return;
    }
    if (enc->enc != NULL) {
        esp_h264_enc_close(enc->enc);
        esp_h264_enc_del(enc->enc);
        enc->enc = NULL;
    }
    if (enc->in_buf != NULL) {
        esp_h264_free(enc->in_buf);
        enc->in_buf = NULL;
    }
    if (enc->out_buf != NULL) {
        esp_h264_free(enc->out_buf);
        enc->out_buf = NULL;
    }
    free(enc);
}

esp_err_t esp_vision_h264_open(int width, int height, int fps, int gop,
                               int bitrate, int qp_min, int qp_max,
                               esp_vision_h264_enc_t **out_enc)
{
    if (out_enc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_enc = NULL;

    // Hardware encoder constraints: 16-aligned, 80..1920 x 80..2032.
    if ((width <= 0) || (height <= 0) || ((width % 16) != 0) || ((height % 16) != 0) ||
            (width < 80) || (width > 1920) || (height < 80) || (height > 2032)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((fps <= 0) || (fps > 255) || (gop <= 0) || (gop > 255) || (bitrate <= 0) ||
            (qp_min < 0) || (qp_max > ESP_H264_QP_MAX) || (qp_min > qp_max)) {
        return ESP_ERR_INVALID_ARG;
    }

    // What the image holds, and what the encoder reads.
    size_t rgb_size = (size_t)width * (size_t)height * sizeof(uint16_t);
    if (rgb_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_vision_h264_enc_t *self = calloc(1, sizeof(*self));
    if (self == NULL) {
        return ESP_ERR_NO_MEM;
    }
    self->w = width;
    self->h = height;
#if VISION_H264_YUV420
    self->raw_size = (size_t)width * (size_t)height * 3 / 2;
#else
    self->raw_size = rgb_size;
#endif

    esp_h264_enc_cfg_hw_t cfg = {
#if VISION_H264_YUV420
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,   // all a pre-revision-3 P4 reads
#else
        .pic_type = ESP_H264_RAW_FMT_RGB565_LE,     // fed straight from image_t (P4 rev >= 3.0)
#endif
        .gop = (uint8_t)gop,
        .fps = (uint8_t)fps,
        .res = {
            .width = (uint16_t)width,
            .height = (uint16_t)height,
        },
        .rc = {
            .bitrate = (uint32_t)bitrate,
            .qp_min = (uint8_t)qp_min,
            .qp_max = (uint8_t)qp_max,
        },
    };

    esp_h264_err_t h264_ret = esp_h264_enc_hw_new(&cfg, &self->enc);
    if (h264_ret != ESP_H264_ERR_OK) {
        self->enc = NULL;
        esp_vision_h264_close(self);
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }
    h264_ret = esp_h264_enc_open(self->enc);
    if (h264_ret != ESP_H264_ERR_OK) {
        esp_vision_h264_close(self);
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }

    // DMA-capable, cache-aligned input; the encoder reads it over DMA.
    self->in_buf = esp_h264_aligned_calloc(16, 1, (uint32_t)self->raw_size, &self->in_buf_size,
                                           ESP_H264_MEM_SPIRAM);
    // Output capacity: an encoded frame never exceeds the RGB565 size.
    self->out_buf = esp_h264_aligned_calloc(16, 1, (uint32_t)rgb_size, &self->out_buf_size,
                                            ESP_H264_MEM_SPIRAM);
    if ((self->in_buf == NULL) || (self->out_buf == NULL)) {
        esp_vision_h264_close(self);
        return ESP_ERR_NO_MEM;
    }

    *out_enc = self;
    return ESP_OK;
}

esp_err_t esp_vision_h264_encode(esp_vision_h264_enc_t *enc, const image_t *img,
                                 uint8_t **nal, size_t *nal_len, int *frame_type)
{
    if ((enc == NULL) || (nal == NULL) || (nal_len == NULL) || (img == NULL) ||
            (img->pixels == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (img->pixfmt != PIXFORMAT_RGB565) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((img->w != enc->w) || (img->h != enc->h)) {
        return ESP_ERR_INVALID_SIZE;
    }

    // The hardware encoder driver does the cache writeback (input) and
    // invalidate (output) around its own DMA internally, so no manual cache
    // maintenance is needed here for these (cacheable PSRAM) buffers.
#if VISION_H264_YUV420
    vision_h264_rgb565_to_uyy_vyy((const uint16_t *)img->pixels, enc->w, enc->h, enc->in_buf);
#else
    memcpy(enc->in_buf, img->pixels, enc->raw_size);
#endif

    esp_h264_enc_in_frame_t in_frame = {
        .raw_data = {
            .buffer = enc->in_buf,
            .len = (uint32_t)enc->raw_size,
        },
        .pts = enc->pts,
    };
    esp_h264_enc_out_frame_t out_frame = {
        .raw_data = {
            .buffer = enc->out_buf,
            .len = enc->out_buf_size,
        },
    };

    esp_h264_err_t h264_ret = esp_h264_enc_process(enc->enc, &in_frame, &out_frame);
    if (h264_ret != ESP_H264_ERR_OK) {
        return esp_vision_h264_err_to_esp_err(h264_ret);
    }

    enc->pts++;
    *nal = out_frame.raw_data.buffer;
    *nal_len = out_frame.length;
    if (frame_type != NULL) {
        *frame_type = (int)out_frame.frame_type;
    }
    return ESP_OK;
}
