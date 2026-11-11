// SPDX-License-Identifier: MIT
//
// A camera sensor described by data rather than code. Camera(sensor=...)
// hands this a dict -- the SCCB address, the chip-ID check, the register
// lists that set it up, its modes, and where its exposure, gain, flip and
// test pattern live -- and it becomes an esp_cam_sensor device like the
// drivers Espressif ships, so the rest of cameraif can't tell the difference.
// Adding a camera is a Python file, not a firmware build.
//
// Everything is copied out of the dict into the C heap: the sensor outlives
// any one Python call, and the soft-reset teardown runs after the Python heap
// is gone. The one exception is a packed register list that already sits in
// flash (a frozen module's bytes): that is read in place, which is why a
// description costs no heap beyond its small control fields.

#include "py/runtime.h"
#include "py/obj.h"

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_IDF_TARGET_ESP32P4) && CONFIG_IDF_TARGET_ESP32P4

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_cam_sensor.h"
#include "esp_memory_utils.h"
#include "esp_sccb_intf.h"

#include "sensor_table.h"

// A register list is packed: three bytes an entry, the register high byte
// first, then its low byte, then the value. A register of 0xFFFF is a wait
// of `value` ms instead. A frozen bytes object is used where it sits in flash;
// anything else is copied, so the list outlives the Python heap.
//
// A register of 0xFFFE is not a register: it makes the next entry a
// read-modify-write, keeping every bit of the register that the `value` of the
// 0xFFFE entry (the mask) doesn't name. One primitive, so a list can set a bit
// without knowing the rest of its register.
#define TABLE_DELAY (0xFFFF)
#define TABLE_MASK (0xFFFE)
#define TABLE_ENTRY (3)
#define TABLE_FIELD (4)     // a control's field: register high, low, shift (signed byte), mask
#define TABLE_EXTRA (8)     // controls beyond exposure and gain (ae_level and the like)

typedef struct {
    const uint8_t *data;
    size_t n;               // entries
    bool owned;             // data is ours to free
} table_list_t;

typedef struct {
    const uint8_t *data;
    size_t len;
    bool owned;
} table_blob_t;

// A control is its registers, "fields", written in order. A field writes
// (value >> shift) & 0xFF, or a left shift for a negative one, into its
// register; a mask other than 0xFF makes that a read-modify-write. A field
// with a zero mask writes the same byte every time (its "shift" is the byte),
// a page register or a group hold. With a table, setting n writes the next
// row of the table, a byte for each field that isn't a constant. `before` and
// `after` are register lists written around the fields.
typedef struct {
    table_blob_t fields, values;
    size_t n_fields, n_cols;    // cols: the fields that take a table byte
    table_list_t before, after; // written around the fields (a group hold, a trigger bit)
    int min, max, def; // max <= 0 counts back from the mode's frame length, times mul
    int mul;
    bool have;
} table_ctrl_t;

typedef struct {
    int id;                 // an esp_cam_sensor parameter id
    table_ctrl_t c;
    int value;
} table_extra_t;

typedef struct {
    esp_cam_sensor_format_t *formats;
    esp_cam_sensor_isp_info_t *isp;
    table_list_t *mode_regs;
    table_list_t *mode_stream;      // two a mode, on then off, when a mode has its own
    size_t n_modes;
    table_list_t init, stream_on, stream_off;
    bool stream_toggle;             // on and off are one write: only when the state changes
    table_ctrl_t exposure, gain;
    table_list_t pattern[2];        // on, off
    bool have_pattern;
    table_list_t vflip_l[2], hmirror_l[2];  // on, off
    bool have_flip_lists;
    table_extra_t extra[TABLE_EXTRA];
    size_t n_extra;
    int flip_reg, flip_v, flip_h;
    int id_regs[4], n_id_regs, id_value;
    table_list_t id_pre;            // written before the ID is read (a page register)
    bool reg16;
    // state
    int exp, gain_v, vflip, hmirror;
    char name[16];
} table_t;

// --- parsing ------------------------------------------------------------

static mp_obj_t table_get(mp_obj_t d, qstr key) {
    mp_map_elem_t *e = mp_map_lookup(mp_obj_dict_get_map(d), MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return e ? e->value : MP_OBJ_NULL;
}

// A key the description can't do without.
static mp_obj_t table_need(mp_obj_t d, qstr key) {
    if (!mp_obj_is_dict_or_ordereddict(d)) {
        mp_raise_TypeError(MP_ERROR_TEXT("a sensor description and its modes are dicts"));
    }
    mp_obj_t v = table_get(d, key);
    if (v == MP_OBJ_NULL || v == mp_const_none) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("sensor description needs '%q'"), key);
    }
    return v;
}

static int table_int(mp_obj_t d, qstr key, int dflt) {
    mp_obj_t v = table_get(d, key);
    return v == MP_OBJ_NULL || v == mp_const_none ? dflt : mp_obj_get_int(v);
}

// Bytes kept where they are if they sit in flash, else copied.
static void table_parse_blob(mp_obj_t b, table_blob_t *out) {
    out->data = NULL;
    out->len = 0;
    out->owned = false;
    size_t len;
    const byte *data = (const byte *)mp_obj_str_get_data(b, &len);
    out->len = len;
    if (esp_ptr_in_drom(data)) {
        out->data = data;
        return;
    }
    uint8_t *copy = malloc(len ? len : 1);
    if (copy == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
    }
    memcpy(copy, data, len);
    out->data = copy;
    out->owned = true;
}

static void table_free_blob(table_blob_t *b) {
    if (b->owned) {
        free((void *)b->data);
    }
}

static void table_free_list(table_list_t *l) {
    if (l->owned) {
        free((void *)l->data);
    }
}

// Either bytes of packed (register, value) entries, or a sequence of
// (register, value) pairs, where (None, ms) waits.
static void table_parse_list(mp_obj_t seq, table_list_t *out) {
    out->data = NULL;
    out->n = 0;
    out->owned = false;
    if (seq == MP_OBJ_NULL || seq == mp_const_none) {
        return;
    }
    if (mp_obj_is_type(seq, &mp_type_bytes)) {
        size_t len;
        const byte *data = (const byte *)mp_obj_str_get_data(seq, &len);
        if (len % TABLE_ENTRY != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("packed sensor registers are three bytes each"));
        }
        out->n = len / TABLE_ENTRY;
        if (esp_ptr_in_drom(data)) {
            out->data = data;
            return;
        }
        uint8_t *copy = malloc(len ? len : 1);
        if (copy == NULL) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
        }
        memcpy(copy, data, len);
        out->data = copy;
        out->owned = true;
        return;
    }
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(seq, &n, &items);
    // A wait over 255 ms takes more than one entry.
    size_t cap = n;
    for (size_t i = 0; i < n; i++) {
        size_t m;
        mp_obj_t *pair;
        mp_obj_get_array(items[i], &m, &pair);
        if (m == 2 && pair[0] == mp_const_none) {
            cap += mp_obj_get_int(pair[1]) / 255;
        }
    }
    uint8_t *w = malloc(cap * TABLE_ENTRY + 1);
    if (w == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
    }
    out->data = w;
    out->owned = true;
    for (size_t i = 0; i < n; i++) {
        size_t m;
        mp_obj_t *pair;
        mp_obj_get_array(items[i], &m, &pair);
        if (m != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("sensor registers are (register, value) pairs"));
        }
        if (pair[0] == mp_const_none) {
            for (int ms = mp_obj_get_int(pair[1]); ms > 0; ms -= 255) {
                *w++ = TABLE_DELAY >> 8;
                *w++ = TABLE_DELAY & 0xFF;
                *w++ = ms > 255 ? 255 : ms;
                out->n++;
            }
        } else {
            int reg = mp_obj_get_int(pair[0]);
            *w++ = reg >> 8;
            *w++ = reg & 0xFF;
            *w++ = mp_obj_get_int(pair[1]);
            out->n++;
        }
    }
}

// A control, as a tuple (register, bytes, minimum, maximum, default) for a
// value spread over consecutive registers, most significant first, or a dict:
//   fields   bytes, four an entry: register high, low, shift, mask
//   table    bytes: what each setting writes into the fields, setting after setting
//   before, after   register lists written around the fields
//   min, max, default, and vts_mul (max and default <= 0 count back from
//   vts_mul times the frame length)
static void table_parse_ctrl(mp_obj_t v, table_ctrl_t *out) {
    memset(out, 0, sizeof(*out));
    out->mul = 1;
    if (v == MP_OBJ_NULL || v == mp_const_none) {
        return;
    }
    if (mp_obj_is_dict_or_ordereddict(v)) {
        mp_obj_t f = table_need(v, MP_QSTR_fields);
        table_parse_blob(f, &out->fields);
        if (out->fields.len == 0 || out->fields.len % TABLE_FIELD != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("control fields are four bytes each"));
        }
        out->n_fields = out->fields.len / TABLE_FIELD;
        for (size_t i = 0; i < out->n_fields; i++) {
            out->n_cols += out->fields.data[i * TABLE_FIELD + 3] != 0;
        }
        mp_obj_t tb = table_get(v, MP_QSTR_table);
        if (tb != MP_OBJ_NULL && tb != mp_const_none) {
            table_parse_blob(tb, &out->values);
            if (out->n_cols == 0 || out->values.len % out->n_cols != 0) {
                mp_raise_ValueError(MP_ERROR_TEXT("control table is a row of bytes for each setting"));
            }
        }
        table_parse_list(table_get(v, MP_QSTR_before), &out->before);
        table_parse_list(table_get(v, MP_QSTR_after), &out->after);
        out->min = mp_obj_get_int(table_need(v, MP_QSTR_min));
        out->max = mp_obj_get_int(table_need(v, MP_QSTR_max));
        int d = table_int(v, MP_QSTR_default, out->max);
        out->def = d;
        out->mul = table_int(v, MP_QSTR_vts_mul, 1);
        out->have = true;
        return;
    }
    size_t n;
    mp_obj_t *it;
    mp_obj_get_array(v, &n, &it);
    if (n != 5) {
        mp_raise_ValueError(MP_ERROR_TEXT("a sensor control is (register, bytes, min, max, default)"));
    }
    int reg = mp_obj_get_int(it[0]);
    int bytes = mp_obj_get_int(it[1]);
    uint8_t *w = malloc(bytes * TABLE_FIELD + 1);
    if (w == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
    }
    out->fields.data = w;
    out->fields.len = bytes * TABLE_FIELD;
    out->fields.owned = true;
    out->n_fields = out->n_cols = bytes;
    for (int i = 0; i < bytes; i++) {
        *w++ = (reg + i) >> 8;
        *w++ = (reg + i) & 0xFF;
        *w++ = 8 * (bytes - 1 - i);
        *w++ = 0xFF;
    }
    out->min = mp_obj_get_int(it[2]);
    out->max = mp_obj_get_int(it[3]);
    out->def = it[4] == mp_const_none ? out->max : mp_obj_get_int(it[4]);
    out->have = true;
}

static void table_free_ctrl(table_ctrl_t *c) {
    table_free_blob(&c->fields);
    table_free_blob(&c->values);
    table_free_list(&c->before);
    table_free_list(&c->after);
}

// A pair of lists, (on, off).
static bool table_parse_pair(mp_obj_t v, table_list_t out[2]) {
    if (v == MP_OBJ_NULL || v == mp_const_none) {
        return false;
    }
    size_t n;
    mp_obj_t *it;
    mp_obj_get_array(v, &n, &it);
    if (n != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("a pair of register lists is (on, off)"));
    }
    table_parse_list(it[0], &out[0]);
    table_parse_list(it[1], &out[1]);
    return true;
}

static int table_bayer(const char *s) {
    static const char *names[] = { "RGGB", "GRBG", "GBRG", "BGGR" };
    static const int vals[] = { ESP_CAM_SENSOR_BAYER_RGGB, ESP_CAM_SENSOR_BAYER_GRBG,
                                ESP_CAM_SENSOR_BAYER_GBRG, ESP_CAM_SENSOR_BAYER_BGGR };
    for (int i = 0; i < 4; i++) {
        if (strcmp(s, names[i]) == 0) {
            return vals[i];
        }
    }
    mp_raise_ValueError(MP_ERROR_TEXT("bayer is one of RGGB, GRBG, GBRG, BGGR"));
}

static int table_pixformat(const char *s) {
    if (strcmp(s, "raw8") == 0) {
        return ESP_CAM_SENSOR_PIXFORMAT_RAW8;
    } else if (strcmp(s, "raw10") == 0) {
        return ESP_CAM_SENSOR_PIXFORMAT_RAW10;
    } else if (strcmp(s, "raw12") == 0) {
        return ESP_CAM_SENSOR_PIXFORMAT_RAW12;
    }
    mp_raise_ValueError(MP_ERROR_TEXT("format is raw8, raw10 or raw12"));
}

static void table_free(table_t *t) {
    if (t == NULL) {
        return;
    }
    for (size_t i = 0; i < t->n_modes; i++) {
        if (t->mode_regs) {
            table_free_list(&t->mode_regs[i]);
        }
        if (t->mode_stream) {
            table_free_list(&t->mode_stream[2 * i]);
            table_free_list(&t->mode_stream[2 * i + 1]);
        }
        if (t->formats) {
            free((void *)t->formats[i].name);
        }
    }
    free(t->formats);
    free(t->isp);
    free(t->mode_regs);
    free(t->mode_stream);
    table_free_list(&t->init);
    table_free_list(&t->id_pre);
    table_free_list(&t->stream_on);
    table_free_list(&t->stream_off);
    table_free_ctrl(&t->exposure);
    table_free_ctrl(&t->gain);
    for (int i = 0; i < 2; i++) {
        table_free_list(&t->pattern[i]);
        table_free_list(&t->vflip_l[i]);
        table_free_list(&t->hmirror_l[i]);
    }
    for (size_t i = 0; i < t->n_extra; i++) {
        table_free_ctrl(&t->extra[i].c);
    }
    free(t);
}

static const struct { qstr key; int id; } table_extra_ids[] = {
    { MP_QSTR_ae_level, ESP_CAM_SENSOR_AE_LEVEL },
    { MP_QSTR_brightness, ESP_CAM_SENSOR_BRIGHTNESS },
    { MP_QSTR_contrast, ESP_CAM_SENSOR_CONTRAST },
    { MP_QSTR_saturation, ESP_CAM_SENSOR_SATURATION },
    { MP_QSTR_sharpness, ESP_CAM_SENSOR_SHARPNESS },
    { MP_QSTR_denoise, ESP_CAM_SENSOR_DENOISE },
    { MP_QSTR_special_effect, ESP_CAM_SENSOR_SPECIAL_EFFECT },
    { MP_QSTR_wb, ESP_CAM_SENSOR_WB },
};

void *cameraif_table_parse(mp_obj_t desc, uint8_t *addr) {
    if (!mp_obj_is_dict_or_ordereddict(desc)) {
        mp_raise_TypeError(MP_ERROR_TEXT("sensor= takes a sensor description dict"));
    }
    table_t *t = calloc(1, sizeof(table_t));
    if (t == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
    }
    // Free it if anything below raises.
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t name = table_get(desc, MP_QSTR_name);
        strncpy(t->name, name ? mp_obj_str_get_str(name) : "sensor", sizeof(t->name) - 1);
        *addr = (uint8_t)mp_obj_get_int(table_need(desc, MP_QSTR_addr));
        t->reg16 = table_int(desc, MP_QSTR_reg_bits, 16) == 16;
        size_t n;
        mp_obj_t *it;
        mp_obj_get_array(table_need(desc, MP_QSTR_id), &n, &it);
        if (n != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("id is (register, value)"));
        }
        // The register is one, or a tuple of up to four, most significant first.
        if (mp_obj_is_int(it[0])) {
            t->id_regs[0] = mp_obj_get_int(it[0]);
            t->id_regs[1] = t->id_regs[0] + 1;
            t->n_id_regs = 2;
        } else {
            mp_obj_t *regs;
            size_t nr;
            mp_obj_get_array(it[0], &nr, &regs);
            if (nr < 1 || nr > 4) {
                mp_raise_ValueError(MP_ERROR_TEXT("id reads one to four registers"));
            }
            t->n_id_regs = nr;
            for (size_t i = 0; i < nr; i++) {
                t->id_regs[i] = mp_obj_get_int(regs[i]);
            }
        }
        t->id_value = mp_obj_get_int(it[1]);
        table_parse_list(table_get(desc, MP_QSTR_id_pre), &t->id_pre);

        table_parse_list(table_get(desc, MP_QSTR_init), &t->init);
        mp_obj_t stream = table_get(desc, MP_QSTR_stream);
        if (stream != MP_OBJ_NULL && stream != mp_const_none) {
            mp_obj_get_array(stream, &n, &it);
            if (n != 2) {
                mp_raise_ValueError(MP_ERROR_TEXT("stream is (registers on, registers off)"));
            }
            table_parse_list(it[0], &t->stream_on);
            table_parse_list(it[1], &t->stream_off);
        }
        t->stream_toggle = table_int(desc, MP_QSTR_stream_toggle, 0) != 0;
        table_parse_ctrl(table_get(desc, MP_QSTR_exposure), &t->exposure);
        table_parse_ctrl(table_get(desc, MP_QSTR_gain), &t->gain);
        // Other controls, by name.
        mp_obj_t ctls = table_get(desc, MP_QSTR_controls);
        if (ctls != MP_OBJ_NULL && ctls != mp_const_none) {
            if (!mp_obj_is_dict_or_ordereddict(ctls)) {
                mp_raise_TypeError(MP_ERROR_TEXT("controls is a dict of controls"));
            }
            mp_map_t *map = mp_obj_dict_get_map(ctls);
            for (size_t i = 0; i < map->alloc; i++) {
                if (!mp_map_slot_is_filled(map, i)) {
                    continue;
                }
                int id = -1;
                for (size_t k = 0; k < MP_ARRAY_SIZE(table_extra_ids); k++) {
                    if (map->table[i].key == MP_OBJ_NEW_QSTR(table_extra_ids[k].key)) {
                        id = table_extra_ids[k].id;
                    }
                }
                if (id < 0 || t->n_extra >= TABLE_EXTRA) {
                    continue;       // a control cameraif has no use for
                }
                table_extra_t *x = &t->extra[t->n_extra++];
                x->id = id;
                table_parse_ctrl(map->table[i].value, &x->c);
                x->value = x->c.def;
            }
        }
        mp_obj_t tp = table_get(desc, MP_QSTR_test_pattern);
        if (tp != MP_OBJ_NULL && tp != mp_const_none) {
            mp_obj_get_array(tp, &n, &it);
            if (n == 4) {       // (register, bytes, on, off): a value over consecutive registers
                int reg = mp_obj_get_int(it[0]);
                int bytes = mp_obj_get_int(it[1]);
                for (int k = 0; k < 2; k++) {
                    int v = mp_obj_get_int(it[2 + k]);
                    uint8_t *w = malloc(bytes * TABLE_ENTRY + 1);
                    if (w == NULL) {
                        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
                    }
                    t->pattern[k].data = w;
                    t->pattern[k].owned = true;
                    t->pattern[k].n = bytes;
                    for (int i = 0; i < bytes; i++) {
                        *w++ = (reg + i) >> 8;
                        *w++ = (reg + i) & 0xFF;
                        *w++ = v >> (8 * (bytes - 1 - i));
                    }
                }
            } else if (n == 2) {    // (list on, list off)
                table_parse_list(it[0], &t->pattern[0]);
                table_parse_list(it[1], &t->pattern[1]);
            } else {
                mp_raise_ValueError(MP_ERROR_TEXT("test_pattern is (register, bytes, on, off) or (on, off)"));
            }
            t->have_pattern = true;
        }
        t->flip_reg = -1;
        mp_obj_t fl = table_get(desc, MP_QSTR_flip);
        if (fl != MP_OBJ_NULL && fl != mp_const_none) {
            if (mp_obj_is_dict_or_ordereddict(fl)) {
                // {"v": (on, off), "h": (on, off)}, lists as for the test pattern
                table_parse_pair(table_get(fl, MP_QSTR_v), t->vflip_l);
                table_parse_pair(table_get(fl, MP_QSTR_h), t->hmirror_l);
                t->have_flip_lists = true;
            } else {
                mp_obj_get_array(fl, &n, &it);       // (register, vertical bits, horizontal bits)
                if (n != 3) {
                    mp_raise_ValueError(MP_ERROR_TEXT("flip is (register, vertical bits, horizontal bits)"));
                }
                t->flip_reg = mp_obj_get_int(it[0]);
                t->flip_v = mp_obj_get_int(it[1]);
                t->flip_h = mp_obj_get_int(it[2]);
            }
        }

        mp_obj_t *ms;
        size_t n_modes;
        mp_obj_get_array(table_need(desc, MP_QSTR_modes), &n_modes, &ms);
        if (n_modes == 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("a sensor needs at least one mode"));
        }
        t->formats = calloc(n_modes, sizeof(esp_cam_sensor_format_t));
        t->isp = calloc(n_modes, sizeof(esp_cam_sensor_isp_info_t));
        t->mode_regs = calloc(n_modes, sizeof(table_list_t));
        t->mode_stream = calloc(n_modes * 2, sizeof(table_list_t));
        if (!t->formats || !t->isp || !t->mode_regs || !t->mode_stream) {
            mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("sensor table"));
        }
        for (size_t i = 0; i < n_modes; i++) {
            mp_obj_t m = ms[i];
            esp_cam_sensor_format_t *f = &t->formats[i];
            t->n_modes = i + 1;     // so a raise below frees what this mode has
            f->name = strdup(mp_obj_str_get_str(table_need(m, MP_QSTR_name)));
            f->format = table_pixformat(mp_obj_str_get_str(table_need(m, MP_QSTR_format)));
            f->port = ESP_CAM_SENSOR_MIPI_CSI;
            f->xclk = table_int(m, MP_QSTR_xclk, 24000000);
            f->width = mp_obj_get_int(table_need(m, MP_QSTR_width));
            f->height = mp_obj_get_int(table_need(m, MP_QSTR_height));
            f->fps = table_int(m, MP_QSTR_fps, 30);
            f->mipi_info.mipi_clk = mp_obj_get_int(table_need(m, MP_QSTR_lane_hz));
            f->mipi_info.lane_num = table_int(m, MP_QSTR_lanes, 2);
            f->mipi_info.line_sync_en = false;
            // version is const in the struct, so the whole thing is copied in.
            const esp_cam_sensor_isp_info_t isp = { .isp_v1_info = {
                .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
                .pclk = table_int(m, MP_QSTR_pclk, 0),
                .hts = table_int(m, MP_QSTR_hts, 0),
                .vts = mp_obj_get_int(table_need(m, MP_QSTR_vts)),
                .bayer_type = table_bayer(mp_obj_str_get_str(table_need(m, MP_QSTR_bayer))),
            } };
            memcpy(&t->isp[i], &isp, sizeof(isp));
            f->isp_info = &t->isp[i];
            table_parse_list(table_get(m, MP_QSTR_regs), &t->mode_regs[i]);
            // A mode that starts and stops the stream its own way.
            table_parse_pair(table_get(m, MP_QSTR_stream), &t->mode_stream[2 * i]);
            f->regs = &t->mode_regs[i];
            f->regs_size = 1;
        }
        nlr_pop();
    } else {
        table_free(t);
        nlr_jump(nlr.ret_val);
    }
    return t;
}

void cameraif_table_free(void *t) {
    table_free((table_t *)t);
}

// --- registers ------------------------------------------------------------

static esp_err_t table_write(esp_cam_sensor_device_t *dev, uint16_t reg, uint8_t val) {
    table_t *t = dev->priv;
    return t->reg16 ? esp_sccb_transmit_reg_a16v8(dev->sccb_handle, reg, val)
                    : esp_sccb_transmit_reg_a8v8(dev->sccb_handle, (uint8_t)reg, val);
}

static esp_err_t table_read(esp_cam_sensor_device_t *dev, uint16_t reg, uint8_t *val) {
    table_t *t = dev->priv;
    return t->reg16 ? esp_sccb_transmit_receive_reg_a16v8(dev->sccb_handle, reg, val)
                    : esp_sccb_transmit_receive_reg_a8v8(dev->sccb_handle, (uint8_t)reg, val);
}

// Only the bits in `mask` change; the rest are read back first.
static esp_err_t table_write_masked(esp_cam_sensor_device_t *dev, uint16_t reg, uint8_t val, uint8_t mask) {
    if (mask != 0xFF) {
        uint8_t old = 0;
        esp_err_t ret = table_read(dev, reg, &old);
        if (ret != ESP_OK) {
            return ret;
        }
        val = (old & ~mask) | (val & mask);
    }
    return table_write(dev, reg, val);
}

static esp_err_t table_write_list(esp_cam_sensor_device_t *dev, const table_list_t *l) {
    int mask = 0xFF;
    for (const uint8_t *p = l->data, *end = p + l->n * TABLE_ENTRY; p < end; p += TABLE_ENTRY) {
        uint16_t reg = p[0] << 8 | p[1];
        if (reg == TABLE_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(p[2]) + 1);
            continue;
        }
        if (reg == TABLE_MASK) {
            mask = p[2];
            continue;
        }
        esp_err_t ret = table_write_masked(dev, reg, p[2], mask);
        mask = 0xFF;
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static int table_vts(esp_cam_sensor_device_t *dev) {
    const esp_cam_sensor_format_t *f = dev->cur_format;
    return f && f->isp_info ? (int)f->isp_info->isp_v1_info.vts : 0;
}

// A limit that is zero or negative counts back from the frame length.
static int table_limit(esp_cam_sensor_device_t *dev, const table_ctrl_t *c, int v) {
    return v > 0 ? v : c->mul * table_vts(dev) + v;
}

static int table_ctrl_max(esp_cam_sensor_device_t *dev, const table_ctrl_t *c) {
    return table_limit(dev, c, c->max);
}

// Write a value through a control's fields.
static esp_err_t table_apply(esp_cam_sensor_device_t *dev, const table_ctrl_t *c, int v) {
    if (!c->have) {
        return ESP_OK;
    }
    const uint8_t *f = c->fields.data;
    int idx = v - c->min;
    if (c->values.data && (idx < 0 || (size_t)(idx + 1) * c->n_cols > c->values.len)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t col = 0;
    esp_err_t ret = table_write_list(dev, &c->before);
    for (size_t i = 0; i < c->n_fields && ret == ESP_OK; i++, f += TABLE_FIELD) {
        uint8_t b;
        if (f[3] == 0) {
            b = f[2];
            ret = table_write(dev, f[0] << 8 | f[1], b);
            continue;
        }
        if (c->values.data) {
            b = c->values.data[idx * c->n_cols + col++];
        } else {
            int sh = (int8_t)f[2];
            b = (uint8_t)(sh >= 0 ? v >> sh : v << -sh);
        }
        ret = table_write_masked(dev, f[0] << 8 | f[1], b, f[3]);
    }
    return ret == ESP_OK ? table_write_list(dev, &c->after) : ret;
}

static esp_err_t table_apply_exposure(esp_cam_sensor_device_t *dev) {
    table_t *t = dev->priv;
    if (!t->exposure.have) {
        return ESP_OK;
    }
    int max = table_ctrl_max(dev, &t->exposure);
    if (t->exp > max) {
        t->exp = max;
    }
    if (t->exp < t->exposure.min) {
        t->exp = t->exposure.min;
    }
    return table_apply(dev, &t->exposure, t->exp);
}

static esp_err_t table_apply_gain(esp_cam_sensor_device_t *dev) {
    table_t *t = dev->priv;
    return table_apply(dev, &t->gain, t->gain_v);
}

static esp_err_t table_apply_orientation(esp_cam_sensor_device_t *dev) {
    table_t *t = dev->priv;
    if (t->have_flip_lists) {
        esp_err_t ret = ESP_OK;
        if (t->vflip_l[0].data) {
            ret = table_write_list(dev, &t->vflip_l[t->vflip ? 0 : 1]);
        }
        if (ret == ESP_OK && t->hmirror_l[0].data) {
            ret = table_write_list(dev, &t->hmirror_l[t->hmirror ? 0 : 1]);
        }
        return ret;
    }
    if (t->flip_reg < 0) {
        return ESP_OK;
    }
    uint8_t v = 0;
    table_read(dev, t->flip_reg, &v);
    v &= ~(t->flip_v | t->flip_h);
    v |= (t->vflip ? t->flip_v : 0) | (t->hmirror ? t->flip_h : 0);
    return table_write(dev, t->flip_reg, v);
}

static bool table_has_flip(const table_t *t) {
    return t->flip_reg >= 0 || (t->have_flip_lists && (t->vflip_l[0].data || t->hmirror_l[0].data));
}

static esp_err_t table_read_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id) {
    table_t *t = dev->priv;
    uint32_t pid = 0;
    esp_err_t ret = table_write_list(dev, &t->id_pre);
    for (int i = 0; i < t->n_id_regs && ret == ESP_OK; i++) {
        uint8_t b = 0;
        ret = table_read(dev, t->id_regs[i], &b);
        pid = pid << 8 | b;
    }
    if (ret == ESP_OK) {
        id->pid = (uint16_t)pid;
    }
    return ret;
}

static table_extra_t *table_find_extra(table_t *t, uint32_t id) {
    for (size_t i = 0; i < t->n_extra; i++) {
        if ((uint32_t)t->extra[i].id == id) {
            return &t->extra[i];
        }
    }
    return NULL;
}

// --- esp_cam_sensor ops ---------------------------------------------------

static esp_err_t table_set_stream(esp_cam_sensor_device_t *dev, int enable) {
    table_t *t = dev->priv;
    const table_list_t *l = enable ? &t->stream_on : &t->stream_off;
    // A mode with its own stream lists uses them.
    if (dev->cur_format) {
        size_t i = (const esp_cam_sensor_format_t *)dev->cur_format - t->formats;
        if (i < t->n_modes && t->mode_stream[2 * i].data) {
            l = &t->mode_stream[2 * i + (enable ? 0 : 1)];
        }
    }
    // A toggle writes the same thing to start and to stop, so only a change counts.
    if (t->stream_toggle && (dev->stream_status != 0) == (enable != 0)) {
        return ESP_OK;
    }
    esp_err_t ret = table_write_list(dev, l);
    if (ret == ESP_OK) {
        dev->stream_status = enable;
    }
    return ret;
}

static esp_err_t table_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format) {
    table_t *t = dev->priv;
    if (format == NULL) {
        format = &t->formats[0];
    }
    esp_err_t ret = table_write_list(dev, &t->init);
    if (ret == ESP_OK) {
        ret = table_write_list(dev, (const table_list_t *)format->regs);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    dev->cur_format = format;
    table_apply_exposure(dev);
    table_apply_gain(dev);
    table_apply_orientation(dev);
    // The other controls keep the values the mode's own list left, until set.
    return table_set_stream(dev, 0);
}

static esp_err_t table_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format) {
    if (dev->cur_format == NULL) {
        return ESP_FAIL;
    }
    memcpy(format, dev->cur_format, sizeof(*format));
    return ESP_OK;
}

static esp_err_t table_query_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats) {
    table_t *t = dev->priv;
    formats->count = t->n_modes;
    formats->format_array = t->formats;
    return ESP_OK;
}

static esp_err_t table_query_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *cap) {
    cap->fmt_raw = 1;
    return ESP_OK;
}

static esp_err_t table_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *q) {
    table_t *t = dev->priv;
    q->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
    q->number.step = 1;
    switch (q->id) {
        case ESP_CAM_SENSOR_EXPOSURE_VAL:
            if (!t->exposure.have) {
                return ESP_ERR_INVALID_ARG;
            }
            q->number.minimum = t->exposure.min;
            q->number.maximum = table_ctrl_max(dev, &t->exposure);
            q->default_value = q->number.maximum;
            return ESP_OK;
        case ESP_CAM_SENSOR_ANGAIN:
            if (!t->gain.have) {
                return ESP_ERR_INVALID_ARG;
            }
            q->number.minimum = t->gain.min;
            q->number.maximum = t->gain.max;
            q->default_value = t->gain.def;
            return ESP_OK;
        case ESP_CAM_SENSOR_VFLIP:
        case ESP_CAM_SENSOR_HMIRROR:
            if (!table_has_flip(t)) {
                return ESP_ERR_INVALID_ARG;
            }
            q->number.minimum = 0;
            q->number.maximum = 1;
            q->default_value = 0;
            return ESP_OK;
        default: {
            table_extra_t *x = table_find_extra(t, q->id);
            if (x == NULL) {
                return ESP_ERR_INVALID_ARG;
            }
            q->number.minimum = x->c.min;
            q->number.maximum = x->c.max;
            q->default_value = x->c.def;
            return ESP_OK;
        }
    }
}

static esp_err_t table_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size) {
    table_t *t = dev->priv;
    int *v = (int *)arg;
    if (size < sizeof(int)) {
        return ESP_ERR_INVALID_SIZE;
    }
    switch (id) {
        case ESP_CAM_SENSOR_EXPOSURE_VAL:
            *v = t->exp;
            return t->exposure.have ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        case ESP_CAM_SENSOR_ANGAIN:
            *v = t->gain_v;
            return t->gain.have ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        case ESP_CAM_SENSOR_VFLIP:
            *v = t->vflip;
            return ESP_OK;
        case ESP_CAM_SENSOR_HMIRROR:
            *v = t->hmirror;
            return ESP_OK;
        default: {
            table_extra_t *x = table_find_extra(t, id);
            if (x == NULL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            *v = x->value;
            return ESP_OK;
        }
    }
}

static esp_err_t table_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size) {
    table_t *t = dev->priv;
    int v = *(const int *)arg;
    switch (id) {
        case ESP_CAM_SENSOR_EXPOSURE_VAL:
            t->exp = v;
            return table_apply_exposure(dev);
        case ESP_CAM_SENSOR_ANGAIN:
            t->gain_v = v < t->gain.min ? t->gain.min : (v > t->gain.max ? t->gain.max : v);
            return table_apply_gain(dev);
        case ESP_CAM_SENSOR_VFLIP:
            t->vflip = v != 0;
            return table_apply_orientation(dev);
        case ESP_CAM_SENSOR_HMIRROR:
            t->hmirror = v != 0;
            return table_apply_orientation(dev);
        default: {
            table_extra_t *x = table_find_extra(t, id);
            if (x == NULL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            x->value = v < x->c.min ? x->c.min : (v > x->c.max ? x->c.max : v);
            return table_apply(dev, &x->c, x->value);
        }
    }
}

static esp_err_t table_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg) {
    table_t *t = dev->priv;
    esp_cam_sensor_reg_val_t *r;
    uint8_t val;
    esp_err_t ret;
    switch (cmd) {
        case ESP_CAM_SENSOR_IOC_HW_RESET:
            if (dev->reset_pin >= 0) {
                gpio_set_level(dev->reset_pin, 0);
                vTaskDelay(pdMS_TO_TICKS(10));
                gpio_set_level(dev->reset_pin, 1);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            return ESP_OK;
        case ESP_CAM_SENSOR_IOC_SW_RESET:
            return ESP_OK;      // the init list starts with the sensor's own reset, if it has one
        case ESP_CAM_SENSOR_IOC_S_STREAM:
            return table_set_stream(dev, *(int *)arg);
        case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
            if (!t->have_pattern) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return table_write_list(dev, &t->pattern[*(int *)arg ? 0 : 1]);
        case ESP_CAM_SENSOR_IOC_S_REG:
            r = (esp_cam_sensor_reg_val_t *)arg;
            return table_write(dev, r->regaddr, r->value);
        case ESP_CAM_SENSOR_IOC_G_REG:
            r = (esp_cam_sensor_reg_val_t *)arg;
            ret = table_read(dev, r->regaddr, &val);
            if (ret == ESP_OK) {
                r->value = val;
            }
            return ret;
        case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
            return table_read_id(dev, arg);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t table_delete(esp_cam_sensor_device_t *dev) {
    if (dev) {
        table_free(dev->priv);
        free(dev);
    }
    return ESP_OK;
}

static const esp_cam_sensor_ops_t table_ops = {
    .query_para_desc = table_query_para_desc,
    .get_para_value = table_get_para_value,
    .set_para_value = table_set_para_value,
    .query_support_formats = table_query_formats,
    .query_support_capability = table_query_capability,
    .set_format = table_set_format,
    .get_format = table_get_format,
    .priv_ioctl = table_priv_ioctl,
    .del = table_delete,
};

// Takes ownership of the table: freed here on failure, by the device's
// delete on success.
esp_cam_sensor_device_t *cameraif_table_detect(void *table, esp_cam_sensor_config_t *config) {
    table_t *t = table;
    esp_cam_sensor_device_t *dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        table_free(t);
        return NULL;
    }
    dev->name = t->name;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    dev->ops = &table_ops;
    dev->priv = t;
    dev->cur_format = &t->formats[0];
    t->exp = table_limit(dev, &t->exposure, t->exposure.def);
    t->gain_v = t->gain.def;

    // A Pi-style camera ties its enable to the connector's GPIO; a board that
    // wires it gives it as pwdn (held low) or reset (held high).
    int pins[2] = { dev->pwdn_pin, dev->reset_pin };
    for (int i = 0; i < 2; i++) {
        if (pins[i] >= 0) {
            gpio_config_t conf = { .pin_bit_mask = 1ULL << pins[i], .mode = GPIO_MODE_OUTPUT };
            gpio_config(&conf);
            gpio_set_level(pins[i], i == 0 ? 0 : 1);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (table_read_id(dev, &dev->id) != ESP_OK || dev->id.pid != t->id_value) {
        table_delete(dev);
        return NULL;
    }
    return dev;
}

#endif // CONFIG_IDF_TARGET_ESP32P4
