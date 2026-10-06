// The ESP32-P4's hardware JPEG engine behind jpegio.JpegEncoder and
// JpegDecoder(hardware=True), when micropython.cmake sees an esp32p4 target
// (JPEGIO_HW=1). Everywhere else this file is two stubs that say no.
//
// The engine reads and writes through the 2D-DMA, so the pixels it reads and
// the bytes it writes live in buffers of its own (jpeg_alloc_*_mem: aligned,
// DMA-capable). A Python buffer is copied in unless it is already plain
// native RGB565 or GRAY in DMA-capable memory; the copy is also where stride
// and swap are applied. Those buffers are kept between calls, so a stream of
// same-sized frames allocates once, and are given back when a much smaller
// image would leave most of one idle.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "jpegio.h"
#include "jpegio_hw.h"

#if JPEGIO_HW

// The QSTR scan only sees this module's include paths, not IDF's.
#ifndef NO_QSTR
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

#define JPEGIO_HW_TIMEOUT_MS 1000

// One aligned, DMA-capable buffer, kept between calls.
typedef struct {
    uint8_t *buf;
    size_t size;
} jpegio_hw_buf_t;

static jpeg_encoder_handle_t jpegio_hw_encoder;
static jpeg_decoder_handle_t jpegio_hw_decoder;
static jpegio_hw_buf_t jpegio_hw_enc_in, jpegio_hw_enc_out, jpegio_hw_dec_in, jpegio_hw_dec_out;

// Makes b at least `need` bytes. Keeps it when it is big enough and not more
// than twice what is asked. false when the memory is not there.
static bool jpegio_hw_reserve(jpegio_hw_buf_t *b, size_t need, bool encoder, int direction) {
    if (b->buf != NULL && b->size >= need && b->size / 2 <= need) {
        return true;
    }
    if (b->buf != NULL) {
        heap_caps_free(b->buf);
        b->buf = NULL;
        b->size = 0;
    }
    size_t got = 0;
    if (encoder) {
        jpeg_encode_memory_alloc_cfg_t cfg = { .buffer_direction = direction };
        b->buf = jpeg_alloc_encoder_mem(need, &cfg, &got);
    } else {
        jpeg_decode_memory_alloc_cfg_t cfg = { .buffer_direction = direction };
        b->buf = jpeg_alloc_decoder_mem(need, &cfg, &got);
    }
    b->size = b->buf != NULL ? got : 0;
    return b->buf != NULL;
}

static bool jpegio_hw_dma_readable(const void *p) {
    return esp_ptr_dma_capable(p) || esp_ptr_dma_ext_capable(p);
}

// --- encode ------------------------------------------------------------------

// One RGB565 pixel as B, G, R bytes in the low 24 bits, each channel widened
// by replicating its top bits (31 -> 255), as a display shows it.
static uint8_t jpegio_hw_w5[32], jpegio_hw_w6[64];

static inline uint32_t jpegio_hw_px(unsigned v) {
    return jpegio_hw_w5[v & 31] | ((uint32_t)jpegio_hw_w6[(v >> 5) & 63] << 8) | ((uint32_t)jpegio_hw_w5[v >> 11] << 16);
}

// RGB565 rows (any stride, native or swapped) to tight 24-bit B, G, R. Four
// pixels at a time make three word stores, the PSRAM's best case; the word
// path needs the source and destination word-aligned, which a tight row of a
// Python buffer is, and anything else takes the byte path.
static void jpegio_hw_widen(const uint8_t *src, int width, int height, size_t stride, bool swap, uint8_t *dst) {
    if (jpegio_hw_w5[31] == 0) {
        for (int i = 0; i < 64; i++) {
            jpegio_hw_w6[i] = (uint8_t)((i << 2) | (i >> 4));
            if (i < 32) {
                jpegio_hw_w5[i] = (uint8_t)((i << 3) | (i >> 2));
            }
        }
    }
    for (int y = 0; y < height; y++, src += stride) {
        const uint8_t *p = src;
        int x = 0;
        if ((((uintptr_t)p | (uintptr_t)dst) & 3) == 0) {
            const uint32_t *pw = (const uint32_t *)p;
            uint32_t *dw = (uint32_t *)dst;
            for (; x + 4 <= width; x += 4, pw += 2, dw += 3) {
                uint32_t a = pw[0], b = pw[1];
                if (swap) {
                    a = ((a & 0x00FF00FF) << 8) | ((a >> 8) & 0x00FF00FF);
                    b = ((b & 0x00FF00FF) << 8) | ((b >> 8) & 0x00FF00FF);
                }
                uint32_t c0 = jpegio_hw_px(a & 0xFFFF), c1 = jpegio_hw_px(a >> 16);
                uint32_t c2 = jpegio_hw_px(b & 0xFFFF), c3 = jpegio_hw_px(b >> 16);
                dw[0] = c0 | (c1 << 24);
                dw[1] = (c1 >> 8) | (c2 << 16);
                dw[2] = (c2 >> 16) | (c3 << 8);
            }
            p = (const uint8_t *)pw;
            dst = (uint8_t *)dw;
        }
        int lo = swap ? 1 : 0;
        for (; x < width; x++, p += 2, dst += 3) {
            uint32_t c = jpegio_hw_px((unsigned)p[lo] | ((unsigned)p[1 - lo] << 8));
            dst[0] = (uint8_t)c;
            dst[1] = (uint8_t)(c >> 8);
            dst[2] = (uint8_t)(c >> 16);
        }
    }
}

mp_obj_t jpegio_hw_encode(const void *pixels, int width, int height, size_t stride,
    int format, bool swap, int quality, bool subsample, bool exact) {
    if (jpegio_hw_encoder == NULL) {
        jpeg_encode_engine_cfg_t eng = { .timeout_ms = JPEGIO_HW_TIMEOUT_MS };
        if (jpeg_new_encoder_engine(&eng, &jpegio_hw_encoder) != ESP_OK) {
            jpegio_hw_encoder = NULL;
            return MP_OBJ_NULL;
        }
    }
    bool gray = format == JPEGIO_FORMAT_GRAY;
    const uint8_t *in = pixels;
    size_t in_len;
    bool rgb888 = !gray && exact;
    if (!rgb888) {
        // GRAY, or RGB565 as the engine reads it: from the buffer itself when
        // it is tight, native and DMA-readable, else from a copy.
        size_t row = (size_t)width * (gray ? 1 : 2);
        in_len = row * (size_t)height;
        if (stride != row || swap || !jpegio_hw_dma_readable(pixels)) {
            if (!jpegio_hw_reserve(&jpegio_hw_enc_in, in_len, true, JPEG_ENC_ALLOC_INPUT_BUFFER)) {
                return MP_OBJ_NULL;
            }
            const uint8_t *src = pixels;
            uint8_t *dst = jpegio_hw_enc_in.buf;
            for (int y = 0; y < height; y++, src += stride, dst += row) {
                if (swap) {
                    for (size_t i = 0; i < row; i += 2) {
                        dst[i] = src[i + 1];
                        dst[i + 1] = src[i];
                    }
                } else {
                    memcpy(dst, src, row);
                }
            }
            in = jpegio_hw_enc_in.buf;
        }
    } else {
        // The engine widens RGB565 by zero-filling (31 -> 248), which leaves
        // every pixel at the bottom of its step: a round trip loses a step
        // whenever the noise is negative, which caps it near 33 dB. It is
        // given RGB888 widened the way a display does it (31 -> 255) instead.
        in_len = (size_t)width * height * 3;
        if (!jpegio_hw_reserve(&jpegio_hw_enc_in, in_len, true, JPEG_ENC_ALLOC_INPUT_BUFFER)) {
            return MP_OBJ_NULL;
        }
        jpegio_hw_widen(pixels, width, height, stride, swap, jpegio_hw_enc_in.buf);
        in = jpegio_hw_enc_in.buf;
    }

    jpeg_encode_cfg_t cfg = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .src_type = gray ? JPEG_ENCODE_IN_FORMAT_GRAY : (rgb888 ? JPEG_ENCODE_IN_FORMAT_RGB888 : JPEG_ENCODE_IN_FORMAT_RGB565),
        .sub_sample = gray ? JPEG_DOWN_SAMPLING_GRAY : (subsample ? JPEG_DOWN_SAMPLING_YUV420 : JPEG_DOWN_SAMPLING_YUV444),
        .image_quality = (uint32_t)quality,
    };
    // A byte a pixel holds nearly every photograph; a noisy one at high
    // quality can need more, and the engine says so rather than overrun, so
    // that one gets a second try with room for anything.
    uint32_t out_len = 0;
    esp_err_t err = ESP_FAIL;
    size_t sizes[2] = { (size_t)width * height + 4096, (size_t)width * height * 4 + 4096 };
    for (int attempt = 0; attempt < 2 && err != ESP_OK; attempt++) {
        if (!jpegio_hw_reserve(&jpegio_hw_enc_out, sizes[attempt], true, JPEG_ENC_ALLOC_OUTPUT_BUFFER)) {
            return MP_OBJ_NULL;
        }
        err = jpeg_encoder_process(jpegio_hw_encoder, &cfg, in, in_len,
            jpegio_hw_enc_out.buf, jpegio_hw_enc_out.size, &out_len);
    }
    if (err != ESP_OK) {
        return MP_OBJ_NULL;
    }
    return mp_obj_new_bytes(jpegio_hw_enc_out.buf, out_len);
}

// --- decode ------------------------------------------------------------------

// The engine's own RGB output goes through the 2D-DMA's BT.601 matrix, which
// is the studio-range one (Y 16..235): JPEG is full range (JFIF), so its RGB
// comes back with black crushed below 16 and white clipped above 235. The
// engine is asked for YUV 4:4:4 instead and converted here with JFIF's
// full-range matrix (libjpeg's integer tables), packed as TJpgDec packs RGB565.
static int16_t jpegio_hw_cr_r[256], jpegio_hw_cb_b[256];
static int32_t jpegio_hw_cr_g[256], jpegio_hw_cb_g[256];

static void jpegio_hw_yuv_tables(void) {
    if (jpegio_hw_cb_b[0] != 0) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        int c = i - 128;
        jpegio_hw_cr_r[i] = (int16_t)((91881 * c + 32768) >> 16);     // 1.402
        jpegio_hw_cb_b[i] = (int16_t)((116130 * c + 32768) >> 16);    // 1.772
        jpegio_hw_cr_g[i] = -46802 * c;                               // -0.714136
        jpegio_hw_cb_g[i] = -22554 * c + 32768;                       // -0.344136, rounding
    }
}

static inline unsigned jpegio_hw_clamp(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : (unsigned)v);
}

const char *jpegio_hw_decode(const uint8_t *data, size_t len, int width, int height,
    uint16_t *pixels, size_t stride, int x, int y) {
    if (jpegio_hw_decoder == NULL) {
        jpeg_decode_engine_cfg_t eng = { .timeout_ms = JPEGIO_HW_TIMEOUT_MS };
        if (jpeg_new_decoder_engine(&eng, &jpegio_hw_decoder) != ESP_OK) {
            jpegio_hw_decoder = NULL;
            return "no JPEG decoder engine";
        }
    }
    jpeg_decode_picture_info_t info;
    if (jpeg_decoder_get_info(data, len, &info) != ESP_OK) {
        return "the engine cannot read this JPEG's header";
    }
    // The engine writes whole MCUs: rows and columns padded to the MCU size.
    int mcux = 8, mcuy = 8;
    if (info.sample_method == JPEG_DOWN_SAMPLING_YUV420) {
        mcux = mcuy = 16;
    } else if (info.sample_method == JPEG_DOWN_SAMPLING_YUV422) {
        mcux = 16;
    }
    size_t pw = ((size_t)width + mcux - 1) / mcux * mcux;
    size_t ph = ((size_t)height + mcuy - 1) / mcuy * mcuy;

    if (!jpegio_hw_dma_readable(data)) {
        if (!jpegio_hw_reserve(&jpegio_hw_dec_in, len, false, JPEG_DEC_ALLOC_INPUT_BUFFER)) {
            return "no memory for the engine's input";
        }
        memcpy(jpegio_hw_dec_in.buf, data, len);
        data = jpegio_hw_dec_in.buf;
    }
    if (!jpegio_hw_reserve(&jpegio_hw_dec_out, pw * ph * 3, false, JPEG_DEC_ALLOC_OUTPUT_BUFFER)) {
        return "no memory for the engine's output";
    }
    jpeg_decode_cfg_t cfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_YUV444,   // see jpegio_hw_yuv_tables
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_len = 0;
    if (jpeg_decoder_process(jpegio_hw_decoder, &cfg, data, len,
        jpegio_hw_dec_out.buf, jpegio_hw_dec_out.size, &out_len) != ESP_OK) {
        return "the engine refused this JPEG (baseline only)";
    }
    if (out_len != pw * ph * 3) {
        return "the engine's output is not the size its MCUs imply";
    }
    jpegio_hw_yuv_tables();
    const uint8_t *src = jpegio_hw_dec_out.buf;      // V, U, Y per pixel
    uint16_t *dst = pixels + (size_t)y * stride + x;
    for (int r = 0; r < height; r++, src += pw * 3, dst += stride) {
        const uint8_t *p = src;
        for (int c = 0; c < width; c++, p += 3) {
            int cr = p[0], cb = p[1], yy = p[2];
            unsigned rr = jpegio_hw_clamp(yy + jpegio_hw_cr_r[cr]);
            unsigned gg = jpegio_hw_clamp(yy + ((jpegio_hw_cb_g[cb] + jpegio_hw_cr_g[cr]) >> 16));
            unsigned bb = jpegio_hw_clamp(yy + jpegio_hw_cb_b[cb]);
            dst[c] = (uint16_t)(((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3));
        }
    }
    return NULL;
}

#else // !JPEGIO_HW

mp_obj_t jpegio_hw_encode(const void *pixels, int width, int height, size_t stride,
    int format, bool swap, int quality, bool subsample, bool exact) {
    return MP_OBJ_NULL;
}

const char *jpegio_hw_decode(const uint8_t *data, size_t len, int width, int height,
    uint16_t *pixels, size_t stride, int x, int y) {
    return "no hardware JPEG decoder on this chip";
}

#endif // JPEGIO_HW
