// SPDX-License-Identifier: MIT
// MIPI DSI host for STM32H747/H757 (DSI host + LTDC; displaydev.FBDisplay).
//
// Same Python interface as the esp32 mipidsi (CircuitPython's names): Bus
// starts the DSI PLL and PHY, Display configures video mode, starts the LTDC
// scanning an RGB565 framebuffer in the GC heap (SDRAM on the Arduino Giga),
// sends the panel's init sequence and exposes the framebuffer through the
// buffer protocol. The LTDC scans the buffer continuously, so refresh() only
// has to push the CPU's dirty cache lines out to memory.
//
// Pixel clock: the LTDC kernel clock is pll3_r_ck. PLL3's VCO is left as the
// firmware set it (P and Q also clock SPI1/2/3), and only the R divider is
// changed while the display runs; it is put back on deinit and soft reset.
//
// Lifecycle: idempotent deinit/__del__/ctor + soft-reset teardown
// (see src/include/displayif/soft_reset.h).

#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "displayif/build.h"
#include "displayif/fb_fill.h"
#include "displayif/soft_reset.h"

#if defined(DSI) && defined(LTDC)

#include "stm32h7xx_hal_dsi.h"
#include "stm32h7xx_hal_ltdc.h"

#define MIPIDSI_INIT_DELAY_FLAG 0x80

typedef struct _mipidsi_bus_obj_t {
    mp_obj_base_t base;
    uint32_t lane_bit_rate_hz;
    uint8_t num_lanes;
    bool deinited;
} mipidsi_bus_obj_t;

typedef struct _mipidsi_display_obj_t {
    mp_obj_base_t base;
    uint8_t *buf;
    size_t buf_len;
    uint32_t pixel_clock_hz;
    uint16_t width;
    uint16_t height;
    uint16_t row_stride;
    bool deinited;
} mipidsi_display_obj_t;

static DSI_HandleTypeDef s_dsi;
static LTDC_HandleTypeDef s_ltdc;
static uint32_t s_pll3_divr_saved; /* 0 = PLL3 R divider untouched */
static bool s_dsi_on;
static bool s_soft_reset_registered;

static const mp_obj_type_t mipidsi_bus_type;
static const mp_obj_type_t mipidsi_display_type;

static void mipidsi_check(HAL_StatusTypeDef st, const char *what) {
    if (st != HAL_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("%s failed (%d)"), what, (int)st);
    }
}

/* Rewrite PLL3's R divider; PLL3 must be off while its dividers change. */
static void mipidsi_pll3_set_divr(uint32_t divr) {
    RCC->CR &= ~RCC_CR_PLL3ON;
    while (RCC->CR & RCC_CR_PLL3RDY) {
    }
    RCC->PLL3DIVR = divr;
    RCC->PLLCFGR |= RCC_PLLCFGR_DIVR3EN;
    RCC->CR |= RCC_CR_PLL3ON;
    while (!(RCC->CR & RCC_CR_PLL3RDY)) {
    }
}

static uint32_t mipidsi_pll3_vco_hz(void) {
    uint32_t src = RCC->PLLCKSELR & RCC_PLLCKSELR_PLLSRC;
    uint32_t in = (src == RCC_PLLCKSELR_PLLSRC_HSE) ? HSE_VALUE
        : (src == RCC_PLLCKSELR_PLLSRC_CSI) ? CSI_VALUE : HSI_VALUE;
    uint32_t m = (RCC->PLLCKSELR & RCC_PLLCKSELR_DIVM3) >> RCC_PLLCKSELR_DIVM3_Pos;
    uint32_t n = ((RCC->PLL3DIVR & RCC_PLL3DIVR_N3) >> RCC_PLL3DIVR_N3_Pos) + 1;
    return m ? in / m * n : 0;
}

/* Stop scanning out and release the DSI host. Safe if already clean. Does not
 * touch GC memory: the framebuffer is swept with the heap. */
static void mipidsi_display_teardown(void) {
    if (LTDC->GCR & LTDC_GCR_LTDCEN) {
        LTDC->GCR &= ~LTDC_GCR_LTDCEN;
    }
    if (s_dsi.Instance != NULL && s_dsi.State == HAL_DSI_STATE_READY) {
        HAL_DSI_Stop(&s_dsi);
    }
    if (s_pll3_divr_saved != 0) {
        mipidsi_pll3_set_divr(s_pll3_divr_saved);
        s_pll3_divr_saved = 0;
    }
}

static void mipidsi_host_teardown(void) {
    mipidsi_display_teardown();
    if (s_dsi_on) {
        HAL_DSI_DeInit(&s_dsi);
        __HAL_RCC_DSI_CLK_DISABLE();
        __HAL_RCC_LTDC_CLK_DISABLE();
        s_dsi_on = false;
    }
}

static void mipidsi_ensure_soft_reset_registered(void) {
    if (!s_soft_reset_registered) {
        displayif_register_soft_reset(mipidsi_host_teardown);
        s_soft_reset_registered = true;
    }
}

/* Bus(*, frequency=500_000_000, num_lanes=2): frequency is the lane bit rate. */
static mp_obj_t mipidsi_bus_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum { ARG_frequency, ARG_num_lanes };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_frequency, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 500000000 } },
        { MP_QSTR_num_lanes, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 2 } },
    };
    mp_arg_val_t vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, vals);
    mp_int_t rate = vals[ARG_frequency].u_int;
    mp_int_t lanes = vals[ARG_num_lanes].u_int;
    if (lanes < 1 || lanes > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("num_lanes must be 1 or 2"));
    }
    /* PLL input HSE/IDF; lane rate = in * NDIV / ODF, VCO = 2 * in * NDIV in
     * 500..1000 MHz. Aim the input at 4 MHz (Arduino's choice on the Giga). */
    uint32_t idf = HSE_VALUE / 4000000;
    if (idf < 1) {
        idf = 1;
    } else if (idf > 7) {
        idf = 7;
    }
    uint32_t in = HSE_VALUE / idf;
    uint32_t odf = 0, ndiv = 0;
    for (odf = 0; odf <= 3; odf++) {
        ndiv = (uint32_t)(((uint64_t)rate << odf) + in / 2) / in;
        uint32_t vco = 2 * in * ndiv;
        if (ndiv >= 10 && ndiv <= 125 && vco >= 500000000 && vco <= 1000000000) {
            break;
        }
    }
    if (odf > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("frequency out of range"));
    }

    mipidsi_host_teardown();
    mipidsi_ensure_soft_reset_registered();

    __HAL_RCC_LTDC_CLK_ENABLE();
    __HAL_RCC_LTDC_FORCE_RESET();
    __HAL_RCC_LTDC_RELEASE_RESET();
    __HAL_RCC_DSI_CLK_ENABLE();
    __HAL_RCC_DSI_FORCE_RESET();
    __HAL_RCC_DSI_RELEASE_RESET();

    memset(&s_dsi, 0, sizeof(s_dsi));
    s_dsi.Instance = DSI;
    s_dsi.Init.NumberOfLanes = (lanes == 2) ? DSI_TWO_DATA_LANES : DSI_ONE_DATA_LANE;
    /* Escape clock (lane byte clock / div) must stay under 20 MHz. */
    uint32_t byte_clk = (uint32_t)(in / 8 * ndiv >> odf);
    s_dsi.Init.TXEscapeCkdiv = (byte_clk + 19999999) / 20000000;
    s_dsi.Init.AutomaticClockLaneControl = DSI_AUTO_CLK_LANE_CTRL_DISABLE;
    DSI_PLLInitTypeDef pll = { .PLLNDIV = ndiv, .PLLIDF = idf, .PLLODF = odf };
    s_dsi_on = true;
    mipidsi_check(HAL_DSI_Init(&s_dsi, &pll), "DSI init");

    mipidsi_bus_obj_t *self = mp_obj_malloc(mipidsi_bus_obj_t, type);
    self->lane_bit_rate_hz = (uint32_t)((uint64_t)in * ndiv >> odf);
    self->num_lanes = (uint8_t)lanes;
    self->deinited = false;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t mipidsi_bus_deinit(mp_obj_t self_in) {
    mipidsi_bus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited) {
        mipidsi_host_teardown();
        self->deinited = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipidsi_bus_deinit_obj, mipidsi_bus_deinit);

static void mipidsi_send_init_sequence(const uint8_t *seq, size_t len) {
    size_t i = 0;
    while (i < len) {
        if (i + 1 >= len) {
            mp_raise_ValueError(MP_ERROR_TEXT("truncated display init sequence"));
        }
        uint8_t cmd = seq[i];
        uint8_t n = seq[i + 1] & ~MIPIDSI_INIT_DELAY_FLAG;
        bool delay = (seq[i + 1] & MIPIDSI_INIT_DELAY_FLAG) != 0;
        if (i + 2 + n + (delay ? 1 : 0) > len) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid display init sequence"));
        }
        const uint8_t *p = &seq[i + 2];
        HAL_StatusTypeDef st;
        if (n == 0) {
            st = HAL_DSI_ShortWrite(&s_dsi, 0, DSI_DCS_SHORT_PKT_WRITE_P0, cmd, 0);
        } else if (n == 1) {
            st = HAL_DSI_ShortWrite(&s_dsi, 0, DSI_DCS_SHORT_PKT_WRITE_P1, cmd, p[0]);
        } else {
            st = HAL_DSI_LongWrite(&s_dsi, 0, DSI_DCS_LONG_PKT_WRITE, n, cmd, (uint8_t *)p);
        }
        mipidsi_check(st, "DSI command");
        uint32_t ms = 10;
        if (delay) {
            ms = p[n];
            if (ms == 255) {
                ms = 500;
            }
        }
        mp_hal_delay_ms(ms);
        i += 2 + n + (delay ? 1 : 0);
    }
}

static mp_obj_t mipidsi_display_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    enum {
        ARG_bus, ARG_init_sequence, ARG_width, ARG_height,
        ARG_hsync_pulse_width, ARG_hsync_back_porch, ARG_hsync_front_porch,
        ARG_vsync_pulse_width, ARG_vsync_back_porch, ARG_vsync_front_porch,
        ARG_pixel_clock_frequency, ARG_virtual_channel, ARG_color_depth,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_bus, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_init_sequence, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_hsync_pulse_width, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_hsync_back_porch, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_hsync_front_porch, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_vsync_pulse_width, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_vsync_back_porch, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_vsync_front_porch, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_pixel_clock_frequency, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_virtual_channel, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_color_depth, MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 16 } },
    };
    mp_arg_val_t v[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, args, MP_ARRAY_SIZE(allowed_args), allowed_args, v);

    if (!mp_obj_is_type(v[ARG_bus].u_obj, &mipidsi_bus_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("bus must be mipidsi.Bus"));
    }
    mipidsi_bus_obj_t *bus = MP_OBJ_TO_PTR(v[ARG_bus].u_obj);
    if (bus->deinited || !s_dsi_on) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("MIPI DSI bus is not initialized"));
    }
    if (v[ARG_color_depth].u_int != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("only color_depth=16 is supported"));
    }
    if (v[ARG_virtual_channel].u_int != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("virtual_channel must be 0"));
    }
    uint32_t w = v[ARG_width].u_int, h = v[ARG_height].u_int;
    uint32_t hsa = v[ARG_hsync_pulse_width].u_int, hbp = v[ARG_hsync_back_porch].u_int;
    uint32_t hfp = v[ARG_hsync_front_porch].u_int;
    uint32_t vsa = v[ARG_vsync_pulse_width].u_int, vbp = v[ARG_vsync_back_porch].u_int;
    uint32_t vfp = v[ARG_vsync_front_porch].u_int;
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || hsa == 0 || vsa == 0
        || v[ARG_pixel_clock_frequency].u_int <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid display timing"));
    }
    mp_buffer_info_t init_info;
    mp_get_buffer_raise(v[ARG_init_sequence].u_obj, &init_info, MP_BUFFER_READ);

    /* Replace any previous Display on this host without dropping the Bus. */
    mipidsi_display_teardown();

    mipidsi_display_obj_t *self = mp_obj_malloc_with_finaliser(mipidsi_display_obj_t, type);
    self->deinited = true;
    self->width = w;
    self->height = h;
    self->row_stride = w * 2;
    self->buf_len = (size_t)self->row_stride * h;
    self->buf = m_malloc(self->buf_len);
    memset(self->buf, 0, self->buf_len);
    SCB_CleanDCache();

    /* Pixel clock: PLL3 VCO / R, nearest R. */
    uint32_t vco = mipidsi_pll3_vco_hz();
    uint32_t want = v[ARG_pixel_clock_frequency].u_int;
    uint32_t r = (vco + want / 2) / want;
    if (r < 1) {
        r = 1;
    } else if (r > 128) {
        r = 128;
    }
    uint32_t pix = vco / r;
    self->pixel_clock_hz = pix;

    /* DSI video mode: horizontal timings are counted in lane byte clocks. */
    uint32_t byte_clk = bus->lane_bit_rate_hz / 8;
    #define TO_BYTE_CLK(px) ((uint32_t)(((uint64_t)(px) * byte_clk + pix / 2) / pix))
    DSI_VidCfgTypeDef vid = {
        .VirtualChannelID = 0,
        .ColorCoding = DSI_RGB565,
        .LooselyPacked = DSI_LOOSELY_PACKED_DISABLE,
        .Mode = DSI_VID_MODE_BURST,
        .PacketSize = w,
        .NumberOfChunks = 0,
        .NullPacketSize = 0xFFF,
        .HSPolarity = DSI_HSYNC_ACTIVE_HIGH,
        .VSPolarity = DSI_VSYNC_ACTIVE_HIGH,
        .DEPolarity = DSI_DATA_ENABLE_ACTIVE_HIGH,
        .HorizontalSyncActive = TO_BYTE_CLK(hsa),
        .HorizontalBackPorch = TO_BYTE_CLK(hbp),
        .HorizontalLine = TO_BYTE_CLK(w + hsa + hbp + hfp),
        .VerticalSyncActive = vsa,
        .VerticalBackPorch = vbp,
        .VerticalFrontPorch = vfp,
        .VerticalActive = h,
        .LPCommandEnable = DSI_LP_COMMAND_ENABLE,
        .LPLargestPacketSize = 16,
        .LPVACTLargestPacketSize = 0,
        .LPHorizontalFrontPorchEnable = DSI_LP_HFP_ENABLE,
        .LPHorizontalBackPorchEnable = DSI_LP_HBP_ENABLE,
        .LPVerticalActiveEnable = DSI_LP_VACT_ENABLE,
        .LPVerticalFrontPorchEnable = DSI_LP_VFP_ENABLE,
        .LPVerticalBackPorchEnable = DSI_LP_VBP_ENABLE,
        .LPVerticalSyncActiveEnable = DSI_LP_VSYNC_ENABLE,
    };
    #undef TO_BYTE_CLK
    mipidsi_check(HAL_DSI_ConfigVideoMode(&s_dsi, &vid), "DSI video mode");
    DSI_PHY_TimerTypeDef phy = {
        .ClockLaneHS2LPTime = 35, .ClockLaneLP2HSTime = 35,
        .DataLaneHS2LPTime = 35, .DataLaneLP2HSTime = 35,
        .DataLaneMaxReadTime = 0, .StopWaitTime = 10,
    };
    mipidsi_check(HAL_DSI_ConfigPhyTimer(&s_dsi, &phy), "DSI PHY timer");

    uint32_t divr = RCC->PLL3DIVR;
    if (s_pll3_divr_saved == 0) {
        s_pll3_divr_saved = divr;
    }
    mipidsi_pll3_set_divr((divr & ~RCC_PLL3DIVR_R3) | ((r - 1) << RCC_PLL3DIVR_R3_Pos));

    memset(&s_ltdc, 0, sizeof(s_ltdc));
    s_ltdc.Instance = LTDC;
    s_ltdc.Init.HSPolarity = LTDC_HSPOLARITY_AH;
    s_ltdc.Init.VSPolarity = LTDC_VSPOLARITY_AH;
    s_ltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
    s_ltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
    s_ltdc.Init.HorizontalSync = hsa - 1;
    s_ltdc.Init.AccumulatedHBP = hsa + hbp - 1;
    s_ltdc.Init.AccumulatedActiveW = hsa + hbp + w - 1;
    s_ltdc.Init.TotalWidth = hsa + hbp + w + hfp - 1;
    s_ltdc.Init.VerticalSync = vsa - 1;
    s_ltdc.Init.AccumulatedVBP = vsa + vbp - 1;
    s_ltdc.Init.AccumulatedActiveH = vsa + vbp + h - 1;
    s_ltdc.Init.TotalHeigh = vsa + vbp + h + vfp - 1;
    mipidsi_check(HAL_LTDC_Init(&s_ltdc), "LTDC init");
    LTDC_LayerCfgTypeDef layer = {
        .WindowX0 = 0, .WindowX1 = w, .WindowY0 = 0, .WindowY1 = h,
        .PixelFormat = LTDC_PIXEL_FORMAT_RGB565,
        .Alpha = 255, .Alpha0 = 0,
        .BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA,
        .BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA,
        .FBStartAdress = (uint32_t)self->buf,
        .ImageWidth = w, .ImageHeight = h,
    };
    mipidsi_check(HAL_LTDC_ConfigLayer(&s_ltdc, &layer, 0), "LTDC layer");

    mipidsi_check(HAL_DSI_Start(&s_dsi), "DSI start");
    mipidsi_send_init_sequence(init_info.buf, init_info.len);
    self->deinited = false;
    return MP_OBJ_FROM_PTR(self);
}

static mipidsi_display_obj_t *mipidsi_live(mp_obj_t self_in) {
    mipidsi_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("MIPI DSI display is deinited"));
    }
    return self;
}

/* The LTDC scans the framebuffer continuously: presenting a frame is pushing
 * dirty D-cache lines out to SDRAM. A whole-cache clean is 1024 set/way
 * operations, cheaper than walking a full-screen range by address. */
static mp_obj_t mipidsi_display_refresh(size_t n_args, const mp_obj_t *args) {
    mipidsi_live(args[0]);
    SCB_CleanDCache();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mipidsi_display_refresh_obj, 1, 5, mipidsi_display_refresh);

static void mipidsi_rect_args(mipidsi_display_obj_t *self, const mp_obj_t *args, int *r) {
    for (int i = 0; i < 4; i++) {
        r[i] = mp_obj_get_int(args[i]);
    }
    if (!displayif_fb_rect_ok(r[0], r[1], r[2], r[3], self->width, self->height)) {
        mp_raise_ValueError(MP_ERROR_TEXT("rect out of range"));
    }
}

/* blit(buf, x, y, w, h): RGB565 rows into the framebuffer. */
static mp_obj_t mipidsi_display_blit(size_t n_args, const mp_obj_t *args) {
    mipidsi_display_obj_t *self = mipidsi_live(args[0]);
    mp_buffer_info_t src;
    mp_get_buffer_raise(args[1], &src, MP_BUFFER_READ);
    int r[4];
    mipidsi_rect_args(self, &args[2], r);
    size_t row_bytes = (size_t)r[2] * 2;
    if (src.len < row_bytes * (size_t)r[3]) {
        mp_raise_ValueError(MP_ERROR_TEXT("blit buffer too small"));
    }
    const uint8_t *s = src.buf;
    uint8_t *d = self->buf + (size_t)r[1] * self->row_stride + (size_t)r[0] * 2;
    for (int row = 0; row < r[3]; row++) {
        memcpy(d, s, row_bytes);
        d += self->row_stride;
        s += row_bytes;
    }
    SCB_CleanDCache();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mipidsi_display_blit_obj, 6, 6, mipidsi_display_blit);

static mp_obj_t mipidsi_display_fill_rect(size_t n_args, const mp_obj_t *args) {
    mipidsi_display_obj_t *self = mipidsi_live(args[0]);
    int r[4];
    mipidsi_rect_args(self, &args[1], r);
    displayif_fb_fill_rect16(self->buf, self->row_stride, r[0], r[1], r[2], r[3],
        (uint16_t)mp_obj_get_int(args[5]));
    SCB_CleanDCache();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mipidsi_display_fill_rect_obj, 6, 6, mipidsi_display_fill_rect);

static mp_obj_t mipidsi_display_deinit(mp_obj_t self_in) {
    mipidsi_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited) {
        mipidsi_display_teardown();
        self->deinited = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mipidsi_display_deinit_obj, mipidsi_display_deinit);

static mp_int_t mipidsi_display_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)flags;
    mipidsi_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    bufinfo->buf = self->deinited ? NULL : self->buf;
    bufinfo->len = self->deinited ? 0 : self->buf_len;
    bufinfo->typecode = 'B';
    return 0;
}

static void mipidsi_display_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mipidsi_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    if (attr == MP_QSTR_width) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->width);
    } else if (attr == MP_QSTR_height) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->height);
    } else if (attr == MP_QSTR_row_stride) {
        dest[0] = MP_OBJ_NEW_SMALL_INT(self->row_stride);
    } else if (attr == MP_QSTR_pixel_clock_frequency) {
        dest[0] = mp_obj_new_int_from_uint(self->pixel_clock_hz);
    } else {
        dest[1] = MP_OBJ_SENTINEL; /* continue lookup in locals_dict */
    }
}

static const mp_rom_map_elem_t mipidsi_bus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mipidsi_bus_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(mipidsi_bus_locals_dict, mipidsi_bus_locals_dict_table);

static const mp_rom_map_elem_t mipidsi_display_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_refresh), MP_ROM_PTR(&mipidsi_display_refresh_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit), MP_ROM_PTR(&mipidsi_display_blit_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill_rect), MP_ROM_PTR(&mipidsi_display_fill_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mipidsi_display_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mipidsi_display_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(mipidsi_display_locals_dict, mipidsi_display_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    mipidsi_display_type,
    MP_QSTR_Display,
    MP_TYPE_FLAG_NONE,
    make_new, mipidsi_display_make,
    attr, mipidsi_display_attr,
    locals_dict, &mipidsi_display_locals_dict,
    buffer, mipidsi_display_get_buffer
    );

static MP_DEFINE_CONST_OBJ_TYPE(
    mipidsi_bus_type,
    MP_QSTR_Bus,
    MP_TYPE_FLAG_NONE,
    make_new, mipidsi_bus_make,
    locals_dict, &mipidsi_bus_locals_dict
    );

static const mp_rom_map_elem_t mipidsi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mipidsi) },
    DISPLAYIF_REVISION_ENTRY,
    { MP_ROM_QSTR(MP_QSTR_Bus), MP_ROM_PTR(&mipidsi_bus_type) },
    { MP_ROM_QSTR(MP_QSTR_Display), MP_ROM_PTR(&mipidsi_display_type) },
};
static MP_DEFINE_CONST_DICT(mipidsi_module_globals, mipidsi_module_globals_table);

const mp_obj_module_t mipidsi_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mipidsi_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mipidsi, mipidsi_user_cmodule);

#endif /* DSI && LTDC */
