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
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif
#endif

struct ppa_mod_client {
    int hw;                 // -1 auto, 0 software, 1 hardware or fail
    bool used_hw;
    #if PPA_HW
    ppa_client_handle_t srm, fill, blend;
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

static bool hw_dma_ok(const void *p) {
    size_t align = 0;
    esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &align);
    return !align || ((uintptr_t)p % align) == 0;
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
    if (!hw_dma_ok(op->dst)) {
        return "the destination buffer isn't aligned for DMA";
    }
    if (hw_client(&c->srm, PPA_OPERATION_SRM) == NULL) {
        return "no PPA client";
    }
    static const ppa_srm_rotation_angle_t angles[4] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
    };
    int rot = ((op->rotate / 90) % 4 + 4) % 4;
    bool turn = rot == 1 || rot == 3;
    uint32_t sw = op->sw ? op->sw : op->src_w, sh = op->sh ? op->sh : op->src_h;
    uint32_t bw = op->w ? op->w : op->dst_w, bh = op->h ? op->h : op->dst_h;
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
            .buffer_size = (uint32_t)op->dst_len,
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
        return "the buffer isn't aligned for DMA";
    }
    if (hw_client(&c->fill, PPA_OPERATION_FILL) == NULL) {
        return "no PPA client";
    }
    ppa_fill_color_mode_t cm = fmt == PPA_MOD_RGB888 ? PPA_FILL_COLOR_MODE_RGB888
        : fmt == PPA_MOD_ARGB8888 ? PPA_FILL_COLOR_MODE_ARGB8888 : PPA_FILL_COLOR_MODE_RGB565;
    ppa_fill_oper_config_t cfg = {
        .out = {
            .buffer = dst, .buffer_size = (uint32_t)dst_len,
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
        return "the destination buffer isn't aligned for DMA";
    }
    if (hw_client(&c->blend, PPA_OPERATION_BLEND) == NULL) {
        return "no PPA client";
    }
    ppa_blend_oper_config_t cfg = {
        .in_bg = { .buffer = bg, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
                   .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = fg, .pic_w = w, .pic_h = h, .block_w = w, .block_h = h,
                   .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .out = { .buffer = dst, .buffer_size = (uint32_t)dst_len, .pic_w = w, .pic_h = h,
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
