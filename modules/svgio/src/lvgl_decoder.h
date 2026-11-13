// SPDX-License-Identifier: MIT
// svgio's LVGL image decoder (lvgl_decoder.c). Compiled only when LVGL is in
// the same build (SVGIO_LVGL_DECODER).
#ifndef SVGIO_LVGL_DECODER_H
#define SVGIO_LVGL_DECODER_H

#include <stdbool.h>
#include <stddef.h>

// The name LVGL reports for it (lv_image_decoder_t.name).
#define SVGIO_LVGL_DECODER_NAME "svgio"

// Register svgio's decoder with the running LVGL, at the head of its list.
// A second call adds nothing. False when LVGL isn't initialised (lv_init()
// first) or the registration couldn't allocate.
bool svgio_lvgl_decoder_register(void);

// True when LVGL is initialised and svgio's decoder is in its list.
bool svgio_lvgl_decoder_registered(void);

// The name of the i-th decoder in LVGL's list, NULL past the end or before
// lv_init(). For svgio.lvgl_decoders().
const char *svgio_lvgl_decoder_name_at(size_t i);

#endif
