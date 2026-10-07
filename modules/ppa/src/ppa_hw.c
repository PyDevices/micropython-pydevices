// ppa's clients, and the ESP32-P4's Pixel Processing Accelerator behind them.
//
// A client holds one PPA client per operation type (the PPA queues one
// transaction per client), registered on first use. srm() runs on the PPA
// when the client allows it and the operation fits the hardware -- every
// format pair, a destination buffer the DMA can write (cache-line aligned) --
// and in software (ppa_sw.c) otherwise. The PPA writes UYVY but not YUY2, so
// YUY2 comes out as UYVY with each byte pair swapped afterwards.

#include <stdlib.h>
#include <string.h>

#include "ppa_mod.h"
#include "ppa_sw.h"

#if PPA_HW
#ifndef NO_QSTR
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"   // esp_cache_get_alignment
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif
#endif

struct ppa_mod_client {
    int hw;                 // -1 auto, 0 software, 1 hardware or fail
    bool used_hw;
    #if PPA_HW
    ppa_client_handle_t srm, fill, blend;
    uint8_t *tmp;           // RGB565 scratch for the hybrid path (see hw_srm)
    size_t tmp_len;
    #endif
};

ppa_mod_client_t *ppa_mod_open(int hw) {
    ppa_mod_client_t *c = calloc(1, sizeof(*c));
    if (c != NULL) {
        c->hw = hw;
    }
    return c;
}

void ppa_mod_close(ppa_mod_client_t *c) {
    if (c == NULL) {
        return;
    }
    #if PPA_HW
    if (c->srm) {
        ppa_unregister_client(c->srm);
    }
    if (c->fill) {
        ppa_unregister_client(c->fill);
    }
    if (c->blend) {
        ppa_unregister_client(c->blend);
    }
    if (c->tmp) {
        heap_caps_free(c->tmp);
    }
    #endif
    free(c);
}

bool ppa_mod_used_hw(ppa_mod_client_t *c) {
    return c->used_hw;
}

#if PPA_HW

static ppa_srm_color_mode_t hw_srm_cm(int fmt, bool out) {
    switch (fmt) {
        case PPA_MOD_RGB565: return PPA_SRM_COLOR_MODE_RGB565;
        case PPA_MOD_RGB888: return PPA_SRM_COLOR_MODE_RGB888;
        case PPA_MOD_ARGB8888: return PPA_SRM_COLOR_MODE_ARGB8888;
        case PPA_MOD_GRAY8: return PPA_SRM_COLOR_MODE_GRAY8;
        case PPA_MOD_YUV420: return PPA_SRM_COLOR_MODE_YUV420;
        case PPA_MOD_UYVY: return PPA_SRM_COLOR_MODE_YUV422_UYVY;
        case PPA_MOD_YUY2: return out ? PPA_SRM_COLOR_MODE_YUV422_UYVY : PPA_SRM_COLOR_MODE_YUV422_YUYV;
    }
    return PPA_SRM_COLOR_MODE_RGB565;
}

static size_t hw_align(void) {
    size_t align = 0;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);
    return align ? align : 1;
}

static bool hw_dma_ok(const void *p) {
    return ((uintptr_t)p % hw_align()) == 0;
}

// The driver wants the destination's length a whole number of cache lines;
// the part of the buffer it may use, or 0 when that doesn't hold the picture.
static uint32_t hw_dma_len(size_t len, size_t pic) {
    size_t n = len - len % hw_align();
    return n >= pic ? (uint32_t)n : 0;
}

// What the PPA makes of scaling n pixels to want: the driver turns the
// factor into a whole part and sixteenths (even sixteenths for YUV output),
// so only some ratios come out at exactly the size asked for.
static bool hw_scales_exactly(uint32_t n, uint32_t want, bool even_frag) {
    float sc = (float)want / (float)n;
    uint32_t i = (uint32_t)sc, f = (uint32_t)(sc * 16) & 15;
    if (even_frag) {
        f &= ~1u;
    }
    return n * i + n * f / 16 == want;
}

static ppa_client_handle_t hw_client(ppa_client_handle_t *slot, ppa_operation_t type) {
    if (*slot == NULL) {
        ppa_client_config_t cfg = { .oper_type = type, .max_pending_trans_num = 1 };
        if (ppa_register_client(&cfg, slot) != ESP_OK) {
            *slot = NULL;
        }
    }
    return *slot;
}

static const char *hw_srm(ppa_mod_client_t *c, const ppa_mod_srm_t *op) {
    static const ppa_srm_rotation_angle_t angles[4] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
    };
    int rot = ((op->rotate / 90) % 4 + 4) % 4;
    bool turn = rot == 1 || rot == 3;
    uint32_t sw = op->sw ? op->sw : op->src_w, sh = op->sh ? op->sh : op->src_h;
    uint32_t bw = op->w ? op->w : op->dst_w, bh = op->h ? op->h : op->dst_h;
    bool yuv420 = op->dst_fmt == PPA_MOD_YUV420, yuv422 = op->dst_fmt == PPA_MOD_YUY2 || op->dst_fmt == PPA_MOD_UYVY;
    // Writing GRAY8 is a hybrid on every P4: the PPA scales (and turns) into
    // RGB565, and one tight pass converts at the final size. The PPA's own
    // GRAY8 is near the plain mean of R, G and B (pure blue comes out 82 where
    // BT.601 luma is 28), so ppa keeps grey as luma everywhere. Before
    // revision 3, IDF gives the PPA no YUV422 or GRAY8 at all: reading them is
    // software's, and writing YUV422 is the same hybrid.
    #if CONFIG_ESP_REV_MIN_FULL < 300
    if (op->src_fmt == PPA_MOD_GRAY8 || op->src_fmt == PPA_MOD_YUY2 || op->src_fmt == PPA_MOD_UYVY) {
        return "a P4 before revision 3: its PPA has no YUV422 or GRAY8";
    }
    bool hybrid = op->dst_fmt == PPA_MOD_GRAY8 || yuv422;
    #else
    bool hybrid = op->dst_fmt == PPA_MOD_GRAY8;
    #endif
    if (hybrid) {
        if (op->src_fmt != PPA_MOD_RGB565) {
            return op->dst_fmt == PPA_MOD_GRAY8 ? "GRAY8 from anything but RGB565 is software's"
                                                : "a P4 before revision 3: its PPA has no YUV422 or GRAY8";
        }
        size_t need = ((size_t)bw * bh * 2 + 63) & ~(size_t)63;
        if (c->tmp_len < need) {
            if (c->tmp) {
                heap_caps_free(c->tmp);
            }
            c->tmp = heap_caps_aligned_calloc(64, 1, need, MALLOC_CAP_SPIRAM);
            c->tmp_len = c->tmp ? need : 0;
            if (c->tmp == NULL) {
                return "no memory for the PPA's scratch picture";
            }
        }
        ppa_mod_srm_t mid = *op;
        mid.dst = c->tmp;
        mid.dst_len = c->tmp_len;
        mid.dst_w = bw;
        mid.dst_h = bh;
        mid.dst_fmt = PPA_MOD_RGB565;
        mid.x = mid.y = 0;
        mid.w = bw;
        mid.h = bh;
        const char *why = hw_srm(c, &mid);
        if (why != NULL) {
            return why;
        }
        // the PPA wrote it by DMA: drop any cached copy before reading
        esp_cache_msync(c->tmp, need, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        ppa_sw_from_565(c->tmp, bw, bw, bh, op);
        return NULL;
    }
    // the PPA writes the destination by DMA (the hybrid above writes its own scratch)
    if (!hw_dma_ok(op->dst)) {
        return "the destination buffer isn't aligned for DMA (64 bytes)";
    }
    uint32_t dma_len = hw_dma_len(op->dst_len, ppa_mod_size(op->dst_fmt, op->dst_w, op->dst_h));
    if (dma_len == 0) {
        return "the destination buffer is too short once cut to whole 64-byte cache lines";
    }
    if (hw_client(&c->srm, PPA_OPERATION_SRM) == NULL) {
        return "no PPA client";
    }
    if (!op->approx && (!hw_scales_exactly(sw, turn ? bh : bw, yuv420 || yuv422) || !hw_scales_exactly(sh, turn ? bw : bh, yuv420))) {
        return "the PPA scales in sixteenths, and this ratio isn't one (approx=True takes the nearest)";
    }
    ppa_color_range_t range = op->yuv_limited ? PPA_COLOR_RANGE_LIMIT : PPA_COLOR_RANGE_FULL;
    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = op->src,
            .pic_w = op->src_w, .pic_h = op->src_h,
            .block_w = sw, .block_h = sh,
            .block_offset_x = op->sx, .block_offset_y = op->sy,
            .srm_cm = hw_srm_cm(op->src_fmt, false),
            .yuv_range = range,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = op->dst,
            .buffer_size = dma_len,
            .pic_w = op->dst_w, .pic_h = op->dst_h,
            .block_offset_x = op->x, .block_offset_y = op->y,
            .srm_cm = hw_srm_cm(op->dst_fmt, true),
            .yuv_range = range,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = angles[rot],
        // rotation follows scaling: these make the rotated result fill the block
        .scale_x = (float)(turn ? bh : bw) / (float)sw,
        .scale_y = (float)(turn ? bw : bh) / (float)sh,
        .mirror_x = op->mirror_x,
        .mirror_y = op->mirror_y,
        .byte_swap = op->swap && op->src_fmt == PPA_MOD_RGB565,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(c->srm, &cfg) != ESP_OK) {
        return "the PPA refused this operation";
    }
    if (op->dst_fmt == PPA_MOD_YUY2) {
        // UYVY -> YUYV: swap each byte pair of the block's rows
        for (uint32_t j = 0; j < bh; j++) {
            uint8_t *d = (uint8_t *)op->dst + ((size_t)(op->y + j) * op->dst_w + op->x) * 2;
            for (uint32_t i = 0; i < bw * 2; i += 2) {
                uint8_t t = d[i];
                d[i] = d[i + 1];
                d[i + 1] = t;
            }
        }
    }
    return NULL;
}

const char *ppa_hw_fill(ppa_mod_client_t *c, void *dst, size_t dst_len, uint32_t dst_w, uint32_t dst_h,
    int fmt, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    if (!hw_dma_ok(dst)) {
        return "the buffer isn't aligned for DMA (64 bytes)";
    }
    uint32_t dma_len = hw_dma_len(dst_len, ppa_mod_size(fmt, dst_w, dst_h));
    if (dma_len == 0) {
        return "the buffer is too short once cut to whole 64-byte cache lines";
    }
    if (hw_client(&c->fill, PPA_OPERATION_FILL) == NULL) {
        return "no PPA client";
    }
    ppa_fill_color_mode_t cm = fmt == PPA_MOD_RGB888 ? PPA_FILL_COLOR_MODE_RGB888
        : fmt == PPA_MOD_ARGB8888 ? PPA_FILL_COLOR_MODE_ARGB8888 : PPA_FILL_COLOR_MODE_RGB565;
    ppa_fill_oper_config_t cfg = {
        .out = {
            .buffer = dst, .buffer_size = dma_len,
            .pic_w = dst_w, .pic_h = dst_h,
            .block_offset_x = x, .block_offset_y = y,
            .fill_cm = cm,
        },
        .fill_block_w = w, .fill_block_h = h,
        .fill_argb_color = { .val = argb },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    c->used_hw = true;
    return ppa_do_fill(c->fill, &cfg) == ESP_OK ? NULL : "the PPA refused this fill";
}

const char *ppa_hw_blend(ppa_mod_client_t *c, const void *bg, const void *fg, void *dst, size_t dst_len,
    uint32_t w, uint32_t h, uint8_t fg_alpha) {
    if (!hw_dma_ok(dst)) {
        return "the destination buffer isn't aligned for DMA (64 bytes)";
    }
    uint32_t dma_len = hw_dma_len(dst_len, (size_t)w * h * 2);
    if (dma_len == 0) {
        return "the destination buffer is too short once cut to whole 64-byte cache lines";
    }
    if (hw_client(&c->blend, PPA_OPERATION_BLEND) == NULL) {
        return "no PPA client";
    }
    ppa_blend_oper_config_t cfg = {
        .in_bg = { .buffer = bg, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
                   .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = fg, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
                   .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .out = { .buffer = dst, .buffer_size = dma_len, .pic_w = w, .pic_h = h,
                 .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .bg_alpha_fix_val = 255,
        .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE, .fg_alpha_fix_val = fg_alpha,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    c->used_hw = true;
    return ppa_do_blend(c->blend, &cfg) == ESP_OK ? NULL : "the PPA refused this blend";
}

#else // !PPA_HW

const char *ppa_hw_fill(ppa_mod_client_t *c, void *dst, size_t dst_len, uint32_t dst_w, uint32_t dst_h,
    int fmt, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb) {
    return "no PPA on this chip (fill is the PPA's alone)";
}

const char *ppa_hw_blend(ppa_mod_client_t *c, const void *bg, const void *fg, void *dst, size_t dst_len,
    uint32_t w, uint32_t h, uint8_t fg_alpha) {
    return "no PPA on this chip (blend is the PPA's alone)";
}

#endif // PPA_HW

const char *ppa_mod_srm(ppa_mod_client_t *c, const ppa_mod_srm_t *op) {
    c->used_hw = false;
    #if PPA_HW
    if (c->hw != 0) {
        const char *why = hw_srm(c, op);
        if (why == NULL) {
            c->used_hw = true;
            return NULL;
        }
        if (c->hw > 0) {
            return why;
        }
    }
    #else
    if (c->hw > 0) {
        return "no PPA on this chip";
    }
    #endif
    return ppa_sw_srm(op);
}
