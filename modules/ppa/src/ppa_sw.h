// ppa's software path (ppa_sw.c), and the checks both paths share.
#ifndef PPA_SW_H
#define PPA_SW_H

#include "ppa_mod.h"

const char *ppa_sw_srm(const ppa_mod_srm_t *op);
const char *ppa_sw_check(const ppa_mod_srm_t *op, size_t src_len);

#ifndef PPA_HW
#define PPA_HW 0
#endif

// The P4's PPA (ppa_hw.c); stubs that say no everywhere else.
const char *ppa_hw_fill(ppa_mod_client_t *c, void *dst, size_t dst_len, uint32_t dst_w, uint32_t dst_h,
    int fmt, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
const char *ppa_hw_blend(ppa_mod_client_t *c, const void *bg, const void *fg, void *dst, size_t dst_len,
    uint32_t w, uint32_t h, uint8_t fg_alpha);

#endif
