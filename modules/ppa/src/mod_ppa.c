// ppa for Python: scale, rotate, mirror, colour-convert, fill and blend.
//
//   ppa.srm(src, 640, 480, dst, 1280, 720)                  # scale to fill dst
//   ppa.srm(src, w, h, dst, h, w, rotate=90)                # turn a quarter, CCW
//   ppa.convert(rgb565, w, h, yuy2, ppa.RGB565, ppa.YUY2)   # same size, new format
//   ppa.fill(dst, w, h, 0xFF8000, x=10, y=10, w=100, h=50)  # RGB888 colour
//   ppa.blend(bg, fg, dst, w, h, 128)                       # fg over bg, alpha 0..255
//
// On the ESP32-P4 the Pixel Processing Accelerator does it; everywhere else
// srm and convert run in portable C (fill and blend are the PPA's alone).
// hardware=None (the default) uses the PPA when it can, False never, True
// insists. srm and convert return whether the PPA did it.

#include <string.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "ppa_mod.h"
#include "ppa_sw.h"

static ppa_mod_client_t *ppa_py_clients[3];     // hardware: auto, never, insist

static ppa_mod_client_t *ppa_py_client(mp_obj_t hw) {
    int mode = hw == mp_const_none ? -1 : (mp_obj_is_true(hw) ? 1 : 0);
    if (mode > 0 && !PPA_HW) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("ppa: no PPA on this chip"));
    }
    ppa_mod_client_t **slot = &ppa_py_clients[mode + 1];
    if (*slot == NULL) {
        *slot = ppa_mod_open(mode);
        if (*slot == NULL) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("ppa: client"));
        }
    }
    return *slot;
}

static void ppa_py_check(const char *why) {
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("ppa: %s"), why);
    }
}

static mp_obj_t ppa_py_run(ppa_mod_client_t *c, ppa_mod_srm_t *op, size_t src_len) {
    ppa_py_check(ppa_sw_check(op, src_len));
    // the PPA is a hardware wait; the software path is long on a big picture
    MP_THREAD_GIL_EXIT();
    const char *why = ppa_mod_srm(c, op);
    MP_THREAD_GIL_ENTER();
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppa: %s"), why);
    }
    return mp_obj_new_bool(ppa_mod_used_hw(c));
}

// srm(src, src_w, src_h, dst, dst_w, dst_h, *, src_fmt, dst_fmt, src_rect, x, y, w, h,
//     rotate, mirror_x, mirror_y, swap, limited, hardware) -> bool
static mp_obj_t ppa_py_srm(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { A_src, A_src_w, A_src_h, A_dst, A_dst_w, A_dst_h, A_src_fmt, A_dst_fmt, A_src_rect,
           A_x, A_y, A_w, A_h, A_rotate, A_mirror_x, A_mirror_y, A_swap, A_limited, A_hardware };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_src_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_src_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dst, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_dst_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dst_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_src_fmt, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PPA_MOD_RGB565} },
        { MP_QSTR_dst_fmt, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PPA_MOD_RGB565} },
        { MP_QSTR_src_rect, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_w, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_h, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_rotate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_mirror_x, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_mirror_y, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_swap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_limited, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_hardware, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_buffer_info_t src, dst;
    mp_get_buffer_raise(a[A_src].u_obj, &src, MP_BUFFER_READ);
    mp_get_buffer_raise(a[A_dst].u_obj, &dst, MP_BUFFER_WRITE);
    ppa_mod_srm_t op = {
        .src = src.buf, .src_w = a[A_src_w].u_int, .src_h = a[A_src_h].u_int, .src_fmt = a[A_src_fmt].u_int,
        .dst = dst.buf, .dst_len = dst.len, .dst_w = a[A_dst_w].u_int, .dst_h = a[A_dst_h].u_int,
        .dst_fmt = a[A_dst_fmt].u_int,
        .x = a[A_x].u_int, .y = a[A_y].u_int, .w = a[A_w].u_int, .h = a[A_h].u_int,
        .rotate = a[A_rotate].u_int, .mirror_x = a[A_mirror_x].u_bool, .mirror_y = a[A_mirror_y].u_bool,
        .swap = a[A_swap].u_bool, .yuv_limited = a[A_limited].u_bool,
    };
    if (a[A_src_rect].u_obj != mp_const_none) {
        mp_obj_t *r;
        mp_obj_get_array_fixed_n(a[A_src_rect].u_obj, 4, &r);
        op.sx = mp_obj_get_int(r[0]);
        op.sy = mp_obj_get_int(r[1]);
        op.sw = mp_obj_get_int(r[2]);
        op.sh = mp_obj_get_int(r[3]);
    }
    return ppa_py_run(ppa_py_client(a[A_hardware].u_obj), &op, src.len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ppa_py_srm_obj, 6, ppa_py_srm);

// convert(src, w, h, dst, src_fmt, dst_fmt, *, swap, limited, hardware) -> bool
static mp_obj_t ppa_py_convert(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { A_src, A_w, A_h, A_dst, A_src_fmt, A_dst_fmt, A_swap, A_limited, A_hardware };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_src, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dst, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_src_fmt, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dst_fmt, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_swap, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_limited, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_hardware, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_buffer_info_t src, dst;
    mp_get_buffer_raise(a[A_src].u_obj, &src, MP_BUFFER_READ);
    mp_get_buffer_raise(a[A_dst].u_obj, &dst, MP_BUFFER_WRITE);
    ppa_mod_srm_t op = {
        .src = src.buf, .src_w = a[A_w].u_int, .src_h = a[A_h].u_int, .src_fmt = a[A_src_fmt].u_int,
        .dst = dst.buf, .dst_len = dst.len, .dst_w = a[A_w].u_int, .dst_h = a[A_h].u_int,
        .dst_fmt = a[A_dst_fmt].u_int, .swap = a[A_swap].u_bool, .yuv_limited = a[A_limited].u_bool,
    };
    return ppa_py_run(ppa_py_client(a[A_hardware].u_obj), &op, src.len);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ppa_py_convert_obj, 6, ppa_py_convert);

// fill(dst, dst_w, dst_h, color, *, x, y, w, h, fmt) -- color is 0xRRGGBB (0xAARRGGBB for ARGB8888)
static mp_obj_t ppa_py_fill(size_t n_args, const mp_obj_t *pos, mp_map_t *kw) {
    enum { A_dst, A_dst_w, A_dst_h, A_color, A_x, A_y, A_w, A_h, A_fmt };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_dst, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_dst_w, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_dst_h, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_color, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_w, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_h, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fmt, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = PPA_MOD_RGB565} },
    };
    mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
    mp_buffer_info_t dst;
    mp_get_buffer_raise(a[A_dst].u_obj, &dst, MP_BUFFER_WRITE);
    uint32_t dw = a[A_dst_w].u_int, dh = a[A_dst_h].u_int, x = a[A_x].u_int, y = a[A_y].u_int;
    uint32_t w = a[A_w].u_int ? (uint32_t)a[A_w].u_int : dw - x, h = a[A_h].u_int ? (uint32_t)a[A_h].u_int : dh - y;
    int fmt = a[A_fmt].u_int;
    if (fmt != PPA_MOD_RGB565 && fmt != PPA_MOD_RGB888 && fmt != PPA_MOD_ARGB8888) {
        mp_raise_ValueError(MP_ERROR_TEXT("ppa: fill takes RGB565, RGB888 or ARGB8888"));
    }
    if (!dw || !dh || !w || !h || x + w > dw || y + h > dh || dst.len < ppa_mod_size(fmt, dw, dh)) {
        mp_raise_ValueError(MP_ERROR_TEXT("ppa: the block or buffer doesn't fit"));
    }
    uint32_t color = (uint32_t)mp_obj_get_int_truncated(a[A_color].u_obj);
    if (fmt != PPA_MOD_ARGB8888) {
        color |= 0xFF000000u;
    }
    ppa_mod_client_t *c = ppa_py_client(mp_const_true);
    MP_THREAD_GIL_EXIT();
    const char *why = ppa_hw_fill(c, dst.buf, dst.len, dw, dh, fmt, x, y, w, h, color);
    MP_THREAD_GIL_ENTER();
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppa: %s"), why);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(ppa_py_fill_obj, 4, ppa_py_fill);

// blend(bg, fg, dst, w, h, alpha) -- RGB565 pictures; fg drawn over bg at alpha 0..255
static mp_obj_t ppa_py_blend(size_t n_args, const mp_obj_t *args) {
    mp_buffer_info_t bg, fg, dst;
    mp_get_buffer_raise(args[0], &bg, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &fg, MP_BUFFER_READ);
    mp_get_buffer_raise(args[2], &dst, MP_BUFFER_WRITE);
    mp_int_t w = mp_obj_get_int(args[3]), h = mp_obj_get_int(args[4]), alpha = mp_obj_get_int(args[5]);
    size_t need = (size_t)w * h * 2;
    if (w < 1 || h < 1 || alpha < 0 || alpha > 255 || bg.len < need || fg.len < need || dst.len < need) {
        mp_raise_ValueError(MP_ERROR_TEXT("ppa: blend takes three w x h RGB565 buffers and alpha 0..255"));
    }
    ppa_mod_client_t *c = ppa_py_client(mp_const_true);
    MP_THREAD_GIL_EXIT();
    const char *why = ppa_hw_blend(c, bg.buf, fg.buf, dst.buf, dst.len, w, h, (uint8_t)alpha);
    MP_THREAD_GIL_ENTER();
    if (why != NULL) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ppa: %s"), why);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ppa_py_blend_obj, 6, 6, ppa_py_blend);

static mp_obj_t ppa_py_size(mp_obj_t fmt, mp_obj_t w, mp_obj_t h) {
    return mp_obj_new_int_from_uint(ppa_mod_size(mp_obj_get_int(fmt), mp_obj_get_int(w), mp_obj_get_int(h)));
}
static MP_DEFINE_CONST_FUN_OBJ_3(ppa_py_size_obj, ppa_py_size);

static const mp_rom_map_elem_t ppa_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ppa) },
    { MP_ROM_QSTR(MP_QSTR_srm), MP_ROM_PTR(&ppa_py_srm_obj) },
    { MP_ROM_QSTR(MP_QSTR_convert), MP_ROM_PTR(&ppa_py_convert_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&ppa_py_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_blend), MP_ROM_PTR(&ppa_py_blend_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&ppa_py_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_HARDWARE), PPA_HW ? MP_ROM_TRUE : MP_ROM_FALSE },   // a PPA in this chip
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(PPA_MOD_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_RGB888), MP_ROM_INT(PPA_MOD_RGB888) },
    { MP_ROM_QSTR(MP_QSTR_ARGB8888), MP_ROM_INT(PPA_MOD_ARGB8888) },
    { MP_ROM_QSTR(MP_QSTR_GRAY8), MP_ROM_INT(PPA_MOD_GRAY8) },
    { MP_ROM_QSTR(MP_QSTR_YUY2), MP_ROM_INT(PPA_MOD_YUY2) },
    { MP_ROM_QSTR(MP_QSTR_UYVY), MP_ROM_INT(PPA_MOD_UYVY) },
    { MP_ROM_QSTR(MP_QSTR_YUV420), MP_ROM_INT(PPA_MOD_YUV420) },
};
static MP_DEFINE_CONST_DICT(ppa_module_globals, ppa_module_globals_table);

const mp_obj_module_t ppa_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ppa_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_ppa, ppa_user_cmodule);
