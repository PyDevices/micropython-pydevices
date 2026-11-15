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
// LVGL asks a decoder for an image's size and then opens it, one after the
// other, and asks again whenever its image cache has let the drawn pixels go.
// The decoder keeps the last SVG it read, parsed and flattened, so the second
// ask and any redraw of the same source cost only the raster.
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

// A file's size in bytes, 0 when it can't be read.
static uint32_t file_size(const char *path) {
    lv_fs_file_t f;
    uint32_t size = 0;
    if (lv_fs_open(&f, path, LV_FS_MODE_RD) == LV_FS_RES_OK) {
        if (lv_fs_seek(&f, 0, LV_FS_SEEK_END) != LV_FS_RES_OK || lv_fs_tell(&f, &size) != LV_FS_RES_OK) {
            size = 0;
        }
        lv_fs_close(&f);
    }
    return size;
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

// --- the SVG it keeps -------------------------------------------------------
//
// One source, parsed, and its outlines once open() has drawn it. It lives on
// the MicroPython heap, where NanoSVG's memory comes from, and is reached from
// a root pointer (svgio_kept_slot()) so the garbage collector keeps it.

typedef struct {
    lv_image_src_t src_type;
    const void *var;            // a variable source: its descriptor,
    const void *data;           // its data and
    uint32_t size;              // its size in bytes, or a file's size
    char *path;                 // a file source: its path
    svgio_image_t *image;
    svgio_prepared_t *prepared; // at 1:1, after the first open()
    int w, h;
} svgio_kept_t;

static svgio_kept_t *kept(void) {
    return *svgio_kept_slot();
}

static void forget(void) {
    svgio_kept_t *k = kept();
    if (k != NULL) {
        *svgio_kept_slot() = NULL;
        svgio_prepared_free(k->prepared);
        svgio_image_free(k->image);
        svgio_mem_free(k->path);
        svgio_mem_free(k);
    }
}

// The kept SVG when it is dsc's source, unchanged.
static svgio_kept_t *find(const lv_image_decoder_dsc_t *dsc) {
    svgio_kept_t *k = kept();
    if (k == NULL || k->src_type != dsc->src_type) {
        return NULL;
    }
    if (dsc->src_type == LV_IMAGE_SRC_VARIABLE) {
        const lv_image_dsc_t *img = dsc->src;
        return k->var == img && k->data == img->data && k->size == img->data_size ? k : NULL;
    }
    if (dsc->src_type == LV_IMAGE_SRC_FILE && strcmp(k->path, dsc->src) == 0 && file_size(dsc->src) == k->size) {
        return k;
    }
    return NULL;
}

// dsc's source parsed, kept in place of the last one. NULL when it isn't SVG,
// doesn't parse to a size, or the heap ran out.
static svgio_kept_t *parse_source(const lv_image_decoder_dsc_t *dsc) {
    svgio_kept_t *k = find(dsc);
    if (k != NULL) {
        return k;
    }
    size_t len = 0;
    char *text = read_source(dsc, &len);
    if (text == NULL) {
        return NULL;
    }
    forget();
    float fw = 0, fh = 0;
    svgio_image_t *image = svgio_parse(text, &fw, &fh);
    lv_free(text);
    if (image == NULL || !(fw > 0 && fh > 0) || fw > 32767 || fh > 32767) {
        LV_LOG_WARN("svgio: the SVG didn't parse to a size");
        svgio_image_free(image);
        return NULL;
    }
    k = svgio_mem_alloc(sizeof(svgio_kept_t));
    const char *path = dsc->src_type == LV_IMAGE_SRC_FILE ? dsc->src : NULL;
    char *copy = path != NULL ? svgio_mem_alloc(strlen(path) + 1) : NULL;
    if (k == NULL || (path != NULL && copy == NULL)) {
        svgio_mem_free(k);
        svgio_image_free(image);
        return NULL;
    }
    memset(k, 0, sizeof(*k));
    k->src_type = dsc->src_type;
    if (path != NULL) {
        strcpy(copy, path);
        k->path = copy;
        k->size = (uint32_t)len;
    } else {
        const lv_image_dsc_t *img = dsc->src;
        k->var = img;
        k->data = img->data;
        k->size = img->data_size;
    }
    k->image = image;
    k->w = (int)(fw + 0.5f);
    k->h = (int)(fh + 0.5f);
    k->w = k->w < 1 ? 1 : k->w;
    k->h = k->h < 1 ? 1 : k->h;
    *svgio_kept_slot() = k;
    return k;
}

// --- LVGL decoder callbacks -------------------------------------------------

static lv_result_t decoder_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc, lv_image_header_t *header) {
    LV_UNUSED(decoder);
    svgio_kept_t *k = parse_source(dsc);
    if (k == NULL) {
        return LV_RESULT_INVALID;
    }
    header->cf = LV_COLOR_FORMAT_RGB565A8;
    header->w = k->w;
    header->h = k->h;
    header->stride = (uint32_t)k->w * 2;
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
        // LVGL's draw buffer is aligned and the stride is even, so every row
        // starts on a 16-bit boundary; the void * says so to -Wcast-align.
        svgio_rgba_to_565(rgba + (size_t)r * width * 4, width,
            (uint16_t *)(void *)(t->pixels + row * t->stride), t->alpha + row * (t->stride / 2));
    }
}

static lv_result_t decoder_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    svgio_kept_t *k = parse_source(dsc);
    if (k == NULL) {
        return LV_RESULT_INVALID;
    }
    if (k->prepared == NULL) {
        k->prepared = svgio_prepare(k->image, 1.0f);
        if (k->prepared == NULL) {
            LV_LOG_WARN("svgio: not enough memory");
            forget();
            return LV_RESULT_INVALID;
        }
    }
    const int w = k->w, h = k->h;
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
        why = svgio_render(k->image, k->prepared, 1.0f, w, h, 0, h, strip, rows, put_rows, &t);
        lv_free(strip);
    }
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
