// SPDX-License-Identifier: MIT
// svgio's LVGL image decoder: an .svg file, or SVG text in an lv_image_dsc_t,
// rendered by svgio's NanoSVG at the SVG's own size, so lv.image shows SVG
// with LVGL's own vector graphics (LV_USE_SVG, ThorVG) left out of the build.
//
// Registration, and open() handing back one decoded draw buffer that LVGL's
// image cache may keep, follow jpegio's decoder (modules/jpegio/src/
// lvgl_decoder.c), which follows LVGL's lv_lodepng.c. The output is
// LV_COLOR_FORMAT_RGB565A8: the colour plane, native-order RGB565, then the
// alpha plane, a byte a pixel, so anti-aliased edges blend over what's behind.
//
// What it claims is what LVGL's own SVG decoder claims: a file whose name
// ends in .svg, or a variable source whose text starts with an XML
// declaration or an <svg> tag. Anything else falls through to the next
// decoder.

#include "lvgl/lvgl.h"
#include "lvgl/src/lvgl_private.h"

#include <string.h>

#include "svgio_core.h"
#include "lvgl_decoder.h"

#if defined(LV_COLOR_16_SWAP) && LV_COLOR_16_SWAP
#error "svgio's LVGL decoder emits native-order RGB565; LV_COLOR_16_SWAP is not supported"
#endif

#define image_cache_draw_buf_handlers &(LV_GLOBAL_DEFAULT()->image_cache_draw_buf_handlers)

// The largest SVG file the decoder reads, so a stray multi-megabyte file
// can't take the heap.
#define SVGIO_LVGL_MAX_FILE (512 * 1024)

static lv_result_t decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header);
static lv_result_t decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc);
static void decoder_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc);

// --- registration -----------------------------------------------------------

static lv_image_decoder_t *find_registered(void) {
    lv_image_decoder_t *dec = NULL;
    while ((dec = lv_image_decoder_get_next(dec)) != NULL) {
        if (dec->info_cb == decoder_info) {
            return dec;
        }
    }
    return NULL;
}

bool svgio_lvgl_decoder_registered(void) {
    return lv_is_initialized() && find_registered() != NULL;
}

const char *svgio_lvgl_decoder_name_at(size_t i) {
    if (!lv_is_initialized()) {
        return NULL;
    }
    lv_image_decoder_t *dec = NULL;
    while ((dec = lv_image_decoder_get_next(dec)) != NULL) {
        if (i-- == 0) {
            return dec->name != NULL ? dec->name : "";
        }
    }
    return NULL;
}

bool svgio_lvgl_decoder_register(void) {
    if (!lv_is_initialized()) {
        return false;
    }
    if (find_registered() != NULL) {
        return true;
    }
    lv_image_decoder_t *dec = lv_image_decoder_create();
    if (dec == NULL) {
        return false;
    }
    lv_image_decoder_set_info_cb(dec, decoder_info);
    lv_image_decoder_set_open_cb(dec, decoder_open);
    lv_image_decoder_set_close_cb(dec, decoder_close);
    dec->name = SVGIO_LVGL_DECODER_NAME;
    return true;
}

// --- the source -------------------------------------------------------------

static bool is_svg_text(const uint8_t *p, size_t n) {
    size_t i = 0;
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        i = 3;          // a UTF-8 byte order mark
    }
    while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) {
        i++;
    }
    return (n - i >= 5 && memcmp(p + i, "<?xml", 5) == 0) || (n - i >= 4 && memcmp(p + i, "<svg", 4) == 0);
}

static bool is_svg_file(const lv_image_decoder_dsc_t *dsc) {
    const char *ext = lv_fs_get_ext(dsc->src);
    return ext != NULL && (strcmp(ext, "svg") == 0 || strcmp(ext, "SVG") == 0);
}

// The SVG's text, NUL-terminated, in a buffer the caller frees with lv_free.
// NULL when the source isn't SVG or can't be read.
static char *read_source(const lv_image_decoder_dsc_t *dsc, size_t *len) {
    char *text = NULL;
    if (dsc->src_type == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t *img = dsc->src;
        if (!is_svg_text(img->data, img->data_size)) {
            return NULL;
        }
        *len = img->data_size;
        text = lv_malloc(*len + 1);
        if (text != NULL) {
            memcpy(text, img->data, *len);
        }
    } else if (dsc->src_type == LV_IMAGE_SRC_FILE && is_svg_file(dsc)) {
        lv_fs_file_t f;
        if (lv_fs_open(&f, dsc->src, LV_FS_MODE_RD) != LV_FS_RES_OK) {
            return NULL;
        }
        uint32_t size = 0, rn = 0;
        if (lv_fs_seek(&f, 0, LV_FS_SEEK_END) == LV_FS_RES_OK && lv_fs_tell(&f, &size) == LV_FS_RES_OK
            && size > 0 && size <= SVGIO_LVGL_MAX_FILE && lv_fs_seek(&f, 0, LV_FS_SEEK_SET) == LV_FS_RES_OK) {
            text = lv_malloc(size + 1);
            if (text != NULL && (lv_fs_read(&f, text, size, &rn) != LV_FS_RES_OK || rn != size)) {
                lv_free(text);
                text = NULL;
            }
        } else if (size > SVGIO_LVGL_MAX_FILE) {
            LV_LOG_WARN("svgio: %s is over %d bytes", (const char *)dsc->src, SVGIO_LVGL_MAX_FILE);
        }
        lv_fs_close(&f);
        *len = size;
    }
    if (text != NULL) {
        text[*len] = '\0';
    }
    return text;
}

// Parse the source. NULL when it isn't SVG, or doesn't parse to a size.
static svgio_image_t *parse_source(const lv_image_decoder_dsc_t *dsc, int *w, int *h) {
    size_t len = 0;
    char *text = read_source(dsc, &len);
    if (text == NULL) {
        return NULL;
    }
    float fw = 0, fh = 0;
    svgio_image_t *image = svgio_parse(text, &fw, &fh);
    lv_free(text);
    if (image == NULL || !(fw > 0 && fh > 0) || fw > 32767 || fh > 32767) {
        LV_LOG_WARN("svgio: the SVG didn't parse to a size");
        svgio_image_free(image);
        return NULL;
    }
    *w = (int)(fw + 0.5f);
    *h = (int)(fh + 0.5f);
    *w = *w < 1 ? 1 : *w;
    *h = *h < 1 ? 1 : *h;
    return image;
}

// --- LVGL decoder callbacks -------------------------------------------------

static lv_result_t decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header) {
    LV_UNUSED(decoder);
    int w, h;
    svgio_image_t *image = parse_source(dsc, &w, &h);
    if (image == NULL) {
        return LV_RESULT_INVALID;
    }
    svgio_image_free(image);
    header->cf = LV_COLOR_FORMAT_RGB565A8;
    header->w = w;
    header->h = h;
    header->stride = (uint32_t)w * 2;
    return LV_RESULT_OK;
}

typedef struct {
    uint8_t *pixels;
    uint8_t *alpha;
    uint32_t stride;        // bytes from one row of the colour plane to the next
} svgio_lvgl_target_t;

static void put_rows(void *ctx, const uint8_t *rgba, int y, int rows, int width) {
    svgio_lvgl_target_t *t = ctx;
    for (int r = 0; r < rows; r++) {
        size_t row = (size_t)(y + r);
        svgio_rgba_to_565(rgba + (size_t)r * width * 4, width,
            (uint16_t *)(t->pixels + row * t->stride), t->alpha + row * (t->stride / 2));
    }
}

static lv_result_t decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    int w, h;
    svgio_image_t *image = parse_source(dsc, &w, &h);
    if (image == NULL) {
        return LV_RESULT_INVALID;
    }
    lv_draw_buf_t *decoded = lv_draw_buf_create_ex(image_cache_draw_buf_handlers, w, h,
        LV_COLOR_FORMAT_RGB565A8, LV_STRIDE_AUTO);
    int rows = SVGIO_STRIP_BYTES / (w * 4);
    rows = rows < 1 ? 1 : rows > h ? h : rows;
    uint8_t *strip = decoded != NULL ? lv_malloc((size_t)w * rows * 4) : NULL;
    const char *why = "not enough memory";
    if (strip != NULL) {
        svgio_lvgl_target_t t = {
            decoded->data, decoded->data + (size_t)decoded->header.stride * h, decoded->header.stride,
        };
        why = svgio_render(image, 1.0f, w, h, strip, rows, put_rows, &t);
        lv_free(strip);
    }
    svgio_image_free(image);
    if (why != NULL) {
        LV_LOG_WARN("svgio: %s", why);
        if (decoded != NULL) {
            lv_draw_buf_destroy(decoded);
        }
        return LV_RESULT_INVALID;
    }

    // From here the same as lv_lodepng.c's decoder_open.
    lv_draw_buf_t *adjusted = lv_image_decoder_post_process(dsc, decoded);
    if (adjusted == NULL) {
        lv_draw_buf_destroy(decoded);
        return LV_RESULT_INVALID;
    }
    if (adjusted != decoded) {
        lv_draw_buf_destroy(decoded);
        decoded = adjusted;
    }
    dsc->decoded = decoded;

    if (dsc->args.no_cache || !lv_image_cache_is_enabled()) {
        return LV_RESULT_OK;
    }
    lv_image_cache_data_t search_key;
    search_key.src_type = dsc->src_type;
    search_key.src = dsc->src;
    search_key.slot.size = decoded->data_size;
    lv_cache_entry_t *entry = lv_image_decoder_add_to_cache(decoder, &search_key, decoded, NULL);
    if (entry == NULL) {
        lv_draw_buf_destroy(decoded);
        dsc->decoded = NULL;
        return LV_RESULT_INVALID;
    }
    dsc->cache_entry = entry;
    return LV_RESULT_OK;
}

static void decoder_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    LV_UNUSED(decoder);
    if (dsc->args.no_cache || !lv_image_cache_is_enabled()) {
        lv_draw_buf_destroy((lv_draw_buf_t *)dsc->decoded);
    }
}
