// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.HIDDevice and btclassic.HIDHost: the board as a Bluetooth
// keyboard, mouse or gamepad, or as the host of one. They wrap the HID half
// of btclassic.h and nothing else, so every backend builds this file as it
// is; mod_btclassic.c registers the types where the backend has HID
// (BTC_HAS_HID).
//
// Reports go in and out as the descriptor lays them out, report ID first
// when it has IDs. The descriptors here (HIDDevice.KEYBOARD, MOUSE, COMBO,
// GAMEPAD) put each kind of report under a fixed ID, the keyboard and mouse
// ones in the boot protocol's layouts, so the helpers that build and decode
// reports know where everything is:
//
//   ID 1 keyboard: modifiers, 0, six key codes; output: the LEDs
//   ID 2 mouse:    buttons, x, y, wheel (signed bytes)
//   ID 3 consumer: one 16-bit usage (volume, play, next ...)
//   ID 4 gamepad:  16 buttons, hat (0..7, 8 centred), x, y, z, rz (signed)

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "btclassic.h"

extern const mp_obj_type_t btc_bluetooth_type;

#define MAX_REPORT 64
#define HOST_DESCRIPTOR_MAX 884

#define ID_KEYBOARD 1
#define ID_MOUSE 2
#define ID_CONSUMER 3
#define ID_GAMEPAD 4

// What a device's descriptor holds, when it's one of ours.
#define HAS_KEYBOARD 1
#define HAS_MOUSE 2
#define HAS_CONSUMER 4
#define HAS_GAMEPAD 8

// ---- the descriptors ----

#define DESC_KEYBOARD \
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, ID_KEYBOARD, \
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, \
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01, \
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, \
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, \
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00, \
    0xc0

#define DESC_CONSUMER \
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, ID_CONSUMER, \
    0x15, 0x00, 0x26, 0xff, 0x03, 0x19, 0x00, 0x2a, 0xff, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, \
    0xc0

#define DESC_MOUSE \
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, ID_MOUSE, 0x09, 0x01, 0xa1, 0x00, \
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, \
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01, \
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, \
    0xc0, 0xc0

#define DESC_GAMEPAD \
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, ID_GAMEPAD, \
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02, \
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01, 0x65, 0x14, \
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, \
    0x65, 0x00, 0x75, 0x04, 0x95, 0x01, 0x81, 0x01, \
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, \
    0xc0

static const uint8_t desc_keyboard[] = { DESC_KEYBOARD, DESC_CONSUMER };
static const uint8_t desc_mouse[] = { DESC_MOUSE };
static const uint8_t desc_combo[] = { DESC_KEYBOARD, DESC_MOUSE, DESC_CONSUMER };
static const uint8_t desc_gamepad[] = { DESC_GAMEPAD };

static MP_DEFINE_BYTES_OBJ(desc_keyboard_obj, desc_keyboard, sizeof(desc_keyboard));
static MP_DEFINE_BYTES_OBJ(desc_mouse_obj, desc_mouse, sizeof(desc_mouse));
static MP_DEFINE_BYTES_OBJ(desc_combo_obj, desc_combo, sizeof(desc_combo));
static MP_DEFINE_BYTES_OBJ(desc_gamepad_obj, desc_gamepad, sizeof(desc_gamepad));

static const struct {
    const uint8_t *desc;
    size_t len;
    uint8_t has;
    uint8_t subclass;
    const char *name;
} ours[] = {
    { desc_keyboard, sizeof(desc_keyboard), HAS_KEYBOARD | HAS_CONSUMER, 0x40, "Keyboard" },
    { desc_mouse, sizeof(desc_mouse), HAS_MOUSE, 0x80, "Mouse" },
    { desc_combo, sizeof(desc_combo), HAS_KEYBOARD | HAS_MOUSE | HAS_CONSUMER, 0xc0, "Keyboard and Mouse" },
    { desc_gamepad, sizeof(desc_gamepad), HAS_GAMEPAD, 0x08, "Gamepad" },
};

static int find_ours(const uint8_t *d, size_t len) {
    for (size_t i = 0; i < MP_ARRAY_SIZE(ours); i++) {
        if (len == ours[i].len && memcmp(d, ours[i].desc, len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Walk a descriptor's short items: whether it has report IDs, and the
// minor device class its Generic Desktop application collections suggest.
static void scan_descriptor(const uint8_t *d, size_t len, bool *ids, uint8_t *subclass) {
    static const uint8_t sizes[4] = {0, 1, 2, 4};
    bool desktop = false;
    uint8_t usage = 0;
    *ids = false;
    *subclass = 0;
    for (size_t i = 0; i < len;) {
        uint8_t prefix = d[i];
        if (prefix == 0xfe) {
            i += 3 + (i + 1 < len ? d[i + 1] : 0);
            continue;
        }
        uint8_t n = sizes[prefix & 3];
        uint8_t v = i + 1 < len && n ? d[i + 1] : 0;
        switch (prefix & 0xfc) {
            case 0x84:
                *ids = true;
                break;
            case 0x04:      // Usage Page
                desktop = v == 0x01 && n == 1;
                break;
            case 0x08:      // Usage
                usage = desktop ? v : 0;
                break;
            case 0xa0:      // Collection
                if (v == 0x01) {    // Application
                    *subclass |= usage == 0x06 || usage == 0x07 ? 0x40
                        : usage == 0x02 ? 0x80
                        : usage == 0x05 ? 0x08
                        : usage == 0x04 ? 0x04 : 0;
                }
                break;
        }
        i += 1 + n;
    }
    if ((*subclass & 0xc0) && (*subclass & 0x0c)) {
        *subclass &= 0xc0;  // a keyboard or mouse with a gamepad: say the first
    }
}

// ---- US keyboard layout ----

// Key codes for ' ' to '~'; bit 7 means with Shift.
static const uint8_t us_keys[95] = {
    0x2c, 0x9e, 0xb4, 0xa0, 0xa1, 0xa2, 0xa4, 0x34, 0xa6, 0xa7, 0xa5, 0xae,
    0x36, 0x2d, 0x37, 0x38, 0x27, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0xb3, 0x33, 0xb6, 0x2e, 0xb7, 0xb8, 0x9f, 0x84, 0x85, 0x86,
    0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x2f,
    0x31, 0x30, 0xa3, 0xad, 0x35, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0xaf, 0xb1, 0xb0, 0xb5,
};

#define MOD_SHIFT 0x02
#define MOD_RIGHT_SHIFT 0x20

// A character as (key code with bit 7 for Shift), or 0 if it has no key.
static uint8_t char_key(uint8_t c) {
    if (c >= 0x20 && c <= 0x7e) {
        return us_keys[c - 0x20];
    }
    return c == '\n' ? 0x28 : c == '\t' ? 0x2b : c == '\b' ? 0x2a : c == 0x1b ? 0x29 : 0;
}

// The character a key types, or 0.
static uint8_t key_char(uint8_t key, uint8_t modifiers) {
    uint8_t shifted = (modifiers & (MOD_SHIFT | MOD_RIGHT_SHIFT)) ? 0x80 : 0;
    for (int i = 0; i < 95; i++) {
        if (us_keys[i] == (key | shifted)) {
            return (uint8_t)(0x20 + i);
        }
    }
    return key == 0x28 || key == 0x58 ? '\n' : key == 0x2b ? '\t' : key == 0x2a ? '\b' : key == 0x29 ? 0x1b : 0;
}

// ---- errors and shared pieces ----

static void hid_raise(int err) {
    switch (err) {
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not connected"));
        case BTC_ERR_ARG:
            mp_raise_ValueError(NULL);
        default:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the Bluetooth stack refused"));
    }
}

static mp_obj_t hid_done(int err) {
    if (err < 0) {
        hid_raise(err);
    }
    return mp_const_none;
}

static void hid_check(bool host) {
    btc_hid_status_t st;
    btc_hid_status(&st);
    if (!st.started || st.host != host) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: HID is stopped"));
    }
}

static bool hid_connected(void) {
    btc_hid_status_t st;
    btc_hid_status(&st);
    return st.connected;
}

static const uint8_t *addr_or_none(mp_obj_t o) {
    if (o == mp_const_none) {
        return NULL;
    }
    mp_buffer_info_t bi;
    mp_get_buffer_raise(o, &bi, MP_BUFFER_READ);
    if (bi.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
    }
    return bi.buf;
}

static void check_bt(mp_obj_t bt) {
    if (!mp_obj_is_type(bt, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    if (!btc_radio_active()) {
        hid_done(btc_radio_start());
    }
}

static int8_t clamp8(mp_int_t v) {
    return (int8_t)(v < -127 ? -127 : v > 127 ? 127 : v);
}

typedef struct {
    mp_obj_base_t base;
} hid_obj_t;

static mp_obj_t hid_changes(mp_obj_t self_in) {
    (void)self_in;
    return mp_obj_new_int_from_uint(btc_hid_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_changes_obj, hid_changes);

static mp_obj_t hid_disconnect(mp_obj_t self_in) {
    (void)self_in;
    return hid_done(btc_hid_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_disconnect_obj, hid_disconnect);

// stats() -> dict: sent, received, dropped, queued
static mp_obj_t hid_stats(mp_obj_t self_in) {
    (void)self_in;
    btc_hid_status_t st;
    btc_hid_status(&st);
    mp_obj_t d = mp_obj_new_dict(4);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_sent), mp_obj_new_int_from_uint(st.sent));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_received), mp_obj_new_int_from_uint(st.received));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_dropped), mp_obj_new_int_from_uint(st.dropped));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_queued), mp_obj_new_int_from_uint(st.queued));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_stats_obj, hid_stats);

static mp_obj_t hid_deinit(mp_obj_t self_in) {
    (void)self_in;
    btc_hid_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hid_deinit_obj, hid_deinit);

static mp_obj_t hid_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return hid_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hid_exit_obj, 4, 4, hid_exit);

// connected, peer, boot: the same on both types.
static bool hid_common_attr(qstr attr, mp_obj_t *dest) {
    btc_hid_status_t st;
    btc_hid_status(&st);
    switch (attr) {
        case MP_QSTR_connected:
            dest[0] = mp_obj_new_bool(st.connected);
            return true;
        case MP_QSTR_peer:
            dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
            return true;
        case MP_QSTR_boot:
            dest[0] = mp_obj_new_bool(st.boot);
            return true;
        default:
            return false;
    }
}

// =========================================================================
// HIDDevice
// =========================================================================

// What the running device's descriptor holds, and the mouse buttons held.
// Module state like the backend's, so a soft reset keeps them.
static uint8_t dev_has;
static uint8_t mouse_buttons;

// HIDDevice(bt, descriptor=HIDDevice.KEYBOARD, *, subclass=None, name=None, boot=None)
static mp_obj_t dev_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_descriptor, ARG_subclass, ARG_name, ARG_boot };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_descriptor, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_PTR(&desc_keyboard_obj)} },
        { MP_QSTR_subclass, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_name, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_boot, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    check_bt(args[ARG_bt].u_obj);
    btc_hid_status_t st;
    btc_hid_status(&st);
    if (st.started && st.host) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("btclassic: an HIDHost is running; deinit() it first (one HID role at a time)"));
    }
    mp_buffer_info_t desc;
    mp_get_buffer_raise(args[ARG_descriptor].u_obj, &desc, MP_BUFFER_READ);
    if (desc.len == 0 || desc.len > HOST_DESCRIPTOR_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("a descriptor is 1 to 884 bytes"));
    }

    int which = find_ours(desc.buf, desc.len);
    bool ids;
    uint8_t subclass;
    scan_descriptor(desc.buf, desc.len, &ids, &subclass);
    if (which >= 0) {
        subclass = ours[which].subclass;
    }
    if (args[ARG_subclass].u_obj != mp_const_none) {
        mp_int_t s = mp_obj_get_int(args[ARG_subclass].u_obj);
        if (s < 0 || s > 0xff) {
            mp_raise_ValueError(MP_ERROR_TEXT("subclass is one byte"));
        }
        subclass = (uint8_t)s;
    }
    if (subclass == 0) {
        subclass = 0x40;
    }
    const char *name;
    if (args[ARG_name].u_obj != mp_const_none) {
        name = mp_obj_str_get_str(args[ARG_name].u_obj);
    } else {
        name = which >= 0 ? ours[which].name
            : (subclass & 0xc0) == 0xc0 ? "Keyboard and Mouse"
            : subclass & 0x80 ? "Mouse"
            : subclass & 0x40 ? "Keyboard" : "Gamepad";
    }
    // Ours lay out the keyboard and mouse as the boot protocol does.
    bool boot = args[ARG_boot].u_obj == mp_const_none
        ? which >= 0 && (ours[which].has & (HAS_KEYBOARD | HAS_MOUSE))
        : mp_obj_is_true(args[ARG_boot].u_obj);

    if (st.started) {
        // A new HIDDevice takes over: the descriptor may have changed.
        btc_hid_stop();
    }
    hid_done(btc_hid_device_start(desc.buf, desc.len, subclass, name, boot));
    dev_has = which >= 0 ? ours[which].has : 0;
    mouse_buttons = 0;
    return MP_OBJ_FROM_PTR(mp_obj_malloc(hid_obj_t, type));
}

static void need(uint8_t what) {
    if (!(dev_has & what)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the descriptor has no such report; use send()"));
    }
}

// Queue one report, waiting while a host is connected for room. False when
// the queue stayed full: no host to drain it.
static bool dev_queue(const uint8_t *r, size_t n) {
    for (;;) {
        int got = btc_hid_device_send(r, n);
        hid_done(got);
        if (got > 0) {
            return true;
        }
        if (!hid_connected()) {
            return false;
        }
        mp_event_wait_ms(5);
    }
}

// connect(addr=None): call a host, or the last one.
static mp_obj_t dev_connect(size_t n_args, const mp_obj_t *args) {
    hid_check(false);
    return hid_done(btc_hid_device_connect(n_args > 1 ? addr_or_none(args[1]) : NULL));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(dev_connect_obj, 1, 2, dev_connect);

// send(report) -> bool: queued, or False when the queue is full.
static mp_obj_t dev_send(mp_obj_t self_in, mp_obj_t report_in) {
    (void)self_in;
    hid_check(false);
    mp_buffer_info_t r;
    mp_get_buffer_raise(report_in, &r, MP_BUFFER_READ);
    if (r.len == 0 || r.len > MAX_REPORT) {
        mp_raise_ValueError(MP_ERROR_TEXT("a report is 1 to 64 bytes"));
    }
    int n = btc_hid_device_send(r.buf, r.len);
    hid_done(n);
    return mp_obj_new_bool(n > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_2(dev_send_obj, dev_send);

static bool send_keys(uint8_t modifiers, const uint8_t *keys, size_t n) {
    uint8_t r[9] = {ID_KEYBOARD, modifiers};
    for (size_t i = 0; i < n && i < 6; i++) {
        r[3 + i] = keys[i];
    }
    return dev_queue(r, sizeof(r));
}

// keyboard(keys=(), modifiers=0) -> bool: the keys held now, up to six key
// codes; keyboard() with nothing releases them all.
static mp_obj_t dev_keyboard(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_keys, ARG_modifiers };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_keys, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_modifiers, MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    hid_check(false);
    need(HAS_KEYBOARD);
    uint8_t keys[6];
    size_t n = 0;
    if (args[ARG_keys].u_obj != mp_const_none) {
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(args[ARG_keys].u_obj, &len, &items);
        if (len > 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("at most six keys at once"));
        }
        for (; n < len; n++) {
            keys[n] = (uint8_t)mp_obj_get_int(items[n]);
        }
    }
    return mp_obj_new_bool(send_keys((uint8_t)args[ARG_modifiers].u_int, keys, n));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dev_keyboard_obj, 1, dev_keyboard);

// type(text) -> characters queued: each one pressed and released, on a US
// layout. Waits for room while a host is connected.
static mp_obj_t dev_type(mp_obj_t self_in, mp_obj_t text_in) {
    (void)self_in;
    hid_check(false);
    need(HAS_KEYBOARD);
    size_t len;
    const uint8_t *s = (const uint8_t *)mp_obj_str_get_data(text_in, &len);
    for (size_t i = 0; i < len; i++) {
        if (!char_key(s[i])) {
            mp_raise_ValueError(MP_ERROR_TEXT("a character with no key on a US keyboard"));
        }
    }
    size_t done = 0;
    for (; done < len; done++) {
        uint8_t k = char_key(s[done]);
        uint8_t key = k & 0x7f;
        if (!send_keys(k & 0x80 ? MOD_SHIFT : 0, &key, 1) || !send_keys(0, NULL, 0)) {
            break;
        }
    }
    return MP_OBJ_NEW_SMALL_INT(done);
}
static MP_DEFINE_CONST_FUN_OBJ_2(dev_type_obj, dev_type);

// mouse(x=0, y=0, *, buttons=None, wheel=0) -> bool: move by x, y (any
// size, sent in steps of 127 at most), turn the wheel, and set the buttons
// (bit 0 left, 1 right, 2 middle), or keep them as they are with None.
static mp_obj_t dev_mouse(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_x, ARG_y, ARG_buttons, ARG_wheel };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_x, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_buttons, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_wheel, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    hid_check(false);
    need(HAS_MOUSE);
    if (args[ARG_buttons].u_obj != mp_const_none) {
        mouse_buttons = (uint8_t)(mp_obj_get_int(args[ARG_buttons].u_obj) & 0x1f);
    }
    mp_int_t x = args[ARG_x].u_int, y = args[ARG_y].u_int, wheel = args[ARG_wheel].u_int;
    do {
        int8_t dx = clamp8(x), dy = clamp8(y), dw = clamp8(wheel);
        uint8_t r[5] = {ID_MOUSE, mouse_buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)dw};
        if (!dev_queue(r, sizeof(r))) {
            return mp_const_false;
        }
        x -= dx;
        y -= dy;
        wheel -= dw;
    } while (x || y || wheel);
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dev_mouse_obj, 1, dev_mouse);

// gamepad(buttons=0, *, x=0, y=0, z=0, rz=0, hat=None) -> bool: the whole
// pad's state. Buttons 1 to 16 are bits 0 to 15; sticks are -127..127;
// hat is 0 (up) to 7 clockwise, None centred.
static mp_obj_t dev_gamepad(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buttons, ARG_x, ARG_y, ARG_z, ARG_rz, ARG_hat };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_buttons, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_z, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_rz, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_hat, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    hid_check(false);
    need(HAS_GAMEPAD);
    mp_int_t hat = args[ARG_hat].u_obj == mp_const_none ? 8 : mp_obj_get_int(args[ARG_hat].u_obj);
    if (hat < 0 || hat > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("hat is 0 to 7, or None"));
    }
    mp_int_t b = args[ARG_buttons].u_int;
    uint8_t r[8] = {
        ID_GAMEPAD, (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)hat,
        (uint8_t)clamp8(args[ARG_x].u_int), (uint8_t)clamp8(args[ARG_y].u_int),
        (uint8_t)clamp8(args[ARG_z].u_int), (uint8_t)clamp8(args[ARG_rz].u_int),
    };
    return mp_obj_new_bool(dev_queue(r, sizeof(r)));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dev_gamepad_obj, 1, dev_gamepad);

// consumer(usage) -> bool: press and release a media key, by its HID
// Consumer usage: 0xcd play/pause, 0xb5 next, 0xb6 previous, 0xe9 volume
// up, 0xea volume down, 0xe2 mute.
static mp_obj_t dev_consumer(mp_obj_t self_in, mp_obj_t usage_in) {
    (void)self_in;
    hid_check(false);
    need(HAS_CONSUMER);
    mp_int_t u = mp_obj_get_int(usage_in);
    if (u < 0 || u > 0x3ff) {
        mp_raise_ValueError(MP_ERROR_TEXT("usage is 0 to 0x3ff"));
    }
    uint8_t down[3] = {ID_CONSUMER, (uint8_t)u, (uint8_t)(u >> 8)};
    uint8_t up[3] = {ID_CONSUMER, 0, 0};
    return mp_obj_new_bool(dev_queue(down, 3) && dev_queue(up, 3));
}
static MP_DEFINE_CONST_FUN_OBJ_2(dev_consumer_obj, dev_consumer);

static void dev_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    if (hid_common_attr(attr, dest)) {
        return;
    }
    if (attr == MP_QSTR_output || attr == MP_QSTR_leds) {
        uint8_t b[MAX_REPORT];
        size_t n = btc_hid_device_output(b, sizeof(b));
        if (attr == MP_QSTR_output) {
            dest[0] = mp_obj_new_bytes(b, n);
        } else {
            // The keyboard's: bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock.
            dest[0] = (dev_has & HAS_KEYBOARD) && n >= 2 && b[0] == ID_KEYBOARD
                ? MP_OBJ_NEW_SMALL_INT(b[1]) : MP_OBJ_NEW_SMALL_INT(0);
        }
        return;
    }
    dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t dev_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&dev_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&hid_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&dev_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_keyboard), MP_ROM_PTR(&dev_keyboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_type), MP_ROM_PTR(&dev_type_obj) },
    { MP_ROM_QSTR(MP_QSTR_mouse), MP_ROM_PTR(&dev_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_gamepad), MP_ROM_PTR(&dev_gamepad_obj) },
    { MP_ROM_QSTR(MP_QSTR_consumer), MP_ROM_PTR(&dev_consumer_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&hid_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&hid_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&hid_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&hid_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_KEYBOARD), MP_ROM_PTR(&desc_keyboard_obj) },
    { MP_ROM_QSTR(MP_QSTR_MOUSE), MP_ROM_PTR(&desc_mouse_obj) },
    { MP_ROM_QSTR(MP_QSTR_COMBO), MP_ROM_PTR(&desc_combo_obj) },
    { MP_ROM_QSTR(MP_QSTR_GAMEPAD), MP_ROM_PTR(&desc_gamepad_obj) },
};
static MP_DEFINE_CONST_DICT(dev_locals, dev_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_hid_device_type,
    MP_QSTR_HIDDevice,
    MP_TYPE_FLAG_NONE,
    make_new, dev_make_new,
    attr, dev_attr,
    locals_dict, &dev_locals
    );

// =========================================================================
// HIDHost
// =========================================================================

// HIDHost(bt)
static mp_obj_t host_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    check_bt(all_args[0]);
    btc_hid_status_t st;
    btc_hid_status(&st);
    if (st.started && !st.host) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("btclassic: an HIDDevice is running; deinit() it first (one HID role at a time)"));
    }
    if (!st.started) {
        hid_done(btc_hid_host_start());
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(hid_obj_t, type));
}

// connect(addr=None, *, boot=False): call a keyboard, mouse or gamepad, or
// the last one. Connected, it switches protocol.
static mp_obj_t host_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_addr, ARG_boot };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_addr, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_boot, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    hid_check(true);
    return hid_done(btc_hid_host_connect(addr_or_none(args[ARG_addr].u_obj), args[ARG_boot].u_bool));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(host_connect_obj, 1, host_connect);

// read() -> bytes or None: the next input report, oldest first.
static mp_obj_t host_read(mp_obj_t self_in) {
    (void)self_in;
    hid_check(true);
    uint8_t b[MAX_REPORT];
    size_t n = btc_hid_host_read(b, sizeof(b));
    return n ? mp_obj_new_bytes(b, n) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(host_read_obj, host_read);

// The connected device's descriptor: whether it has IDs, and which of ours
// it is (-1 for none). A copy up to 256 bytes is enough to tell: ours are
// shorter, and the item walk only needs to find one ID.
static void host_layout(bool *ids, int *which) {
    uint8_t d[256];
    size_t n = btc_hid_host_descriptor(d, sizeof(d));
    uint8_t subclass;
    scan_descriptor(d, n, ids, &subclass);
    *which = n < sizeof(d) ? find_ours(d, n) : -1;
}

// send(report): an output report, report ID first when the descriptor has
// IDs (or the link runs the boot protocol): a keyboard's LEDs are
// bytes((1, leds)).
static mp_obj_t host_send(mp_obj_t self_in, mp_obj_t report_in) {
    (void)self_in;
    hid_check(true);
    mp_buffer_info_t r;
    mp_get_buffer_raise(report_in, &r, MP_BUFFER_READ);
    if (r.len == 0 || r.len > MAX_REPORT) {
        mp_raise_ValueError(MP_ERROR_TEXT("a report is 1 to 64 bytes"));
    }
    btc_hid_status_t st;
    btc_hid_status(&st);
    bool ids;
    int which;
    host_layout(&ids, &which);
    const uint8_t *p = r.buf;
    if (ids || st.boot) {
        return hid_done(btc_hid_host_send(p[0], p + 1, r.len - 1));
    }
    return hid_done(btc_hid_host_send(0, p, r.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(host_send_obj, host_send);

static mp_obj_t signed_byte(uint8_t b) {
    return MP_OBJ_NEW_SMALL_INT((int8_t)b);
}

// decode(report) -> tuple or None: a report in a layout this module knows,
// taken apart. Those are the boot protocol's keyboard and mouse, and every
// report of a device whose descriptor is one of HIDDevice's.
//   ("keyboard", modifiers, (key, ...))
//   ("mouse", buttons, x, y, wheel)
//   ("consumer", usage)
//   ("gamepad", buttons, hat, x, y, z, rz)     hat None when centred
static mp_obj_t host_decode(mp_obj_t self_in, mp_obj_t report_in) {
    (void)self_in;
    mp_buffer_info_t r;
    mp_get_buffer_raise(report_in, &r, MP_BUFFER_READ);
    const uint8_t *p = r.buf;
    if (r.len < 2) {
        return mp_const_none;
    }
    btc_hid_status_t st;
    btc_hid_status(&st);
    bool ids;
    int which = -1;
    if (!st.boot) {
        host_layout(&ids, &which);
        if (which < 0) {
            return mp_const_none;
        }
    }
    uint8_t has = st.boot ? (HAS_KEYBOARD | HAS_MOUSE) : ours[which].has;
    if (p[0] == ID_KEYBOARD && (has & HAS_KEYBOARD) && r.len >= 9) {
        mp_obj_t keys[6];
        size_t n = 0;
        for (int i = 3; i < 9; i++) {
            if (p[i] > 0x03) {      // 1..3 are the error codes, 0 is none
                keys[n++] = MP_OBJ_NEW_SMALL_INT(p[i]);
            }
        }
        mp_obj_t t[3] = {MP_OBJ_NEW_QSTR(MP_QSTR_keyboard), MP_OBJ_NEW_SMALL_INT(p[1]), mp_obj_new_tuple(n, keys)};
        return mp_obj_new_tuple(3, t);
    }
    if (p[0] == ID_MOUSE && (has & HAS_MOUSE) && r.len >= 4) {
        mp_obj_t t[5] = {MP_OBJ_NEW_QSTR(MP_QSTR_mouse), MP_OBJ_NEW_SMALL_INT(p[1] & 0x1f),
                         signed_byte(p[2]), signed_byte(p[3]), r.len >= 5 ? signed_byte(p[4]) : MP_OBJ_NEW_SMALL_INT(0)};
        return mp_obj_new_tuple(5, t);
    }
    if (p[0] == ID_CONSUMER && (has & HAS_CONSUMER) && r.len >= 3) {
        mp_obj_t t[2] = {MP_OBJ_NEW_QSTR(MP_QSTR_consumer), MP_OBJ_NEW_SMALL_INT(p[1] | (p[2] << 8))};
        return mp_obj_new_tuple(2, t);
    }
    if (p[0] == ID_GAMEPAD && (has & HAS_GAMEPAD) && r.len >= 8) {
        uint8_t hat = p[3] & 0x0f;
        mp_obj_t t[7] = {MP_OBJ_NEW_QSTR(MP_QSTR_gamepad), MP_OBJ_NEW_SMALL_INT(p[1] | (p[2] << 8)),
                         hat <= 7 ? MP_OBJ_NEW_SMALL_INT(hat) : mp_const_none,
                         signed_byte(p[4]), signed_byte(p[5]), signed_byte(p[6]), signed_byte(p[7])};
        return mp_obj_new_tuple(7, t);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(host_decode_obj, host_decode);

// char(key, modifiers=0) -> str or None: what a key types on a US layout.
static mp_obj_t host_char(size_t n_args, const mp_obj_t *args) {
    mp_int_t key = mp_obj_get_int(args[0]);
    mp_int_t mods = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    uint8_t c = key > 0 && key < 0x100 ? key_char((uint8_t)key, (uint8_t)mods) : 0;
    return c ? mp_obj_new_str((const char *)&c, 1) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(host_char_fun_obj, 1, 2, host_char);
static MP_DEFINE_CONST_STATICMETHOD_OBJ(host_char_obj, MP_ROM_PTR(&host_char_fun_obj));

static void host_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    if (hid_common_attr(attr, dest)) {
        return;
    }
    if (attr == MP_QSTR_descriptor) {
        uint8_t *b = m_new(uint8_t, HOST_DESCRIPTOR_MAX);
        size_t n = btc_hid_host_descriptor(b, HOST_DESCRIPTOR_MAX);
        dest[0] = n ? mp_obj_new_bytes(b, n) : mp_const_none;
        m_del(uint8_t, b, HOST_DESCRIPTOR_MAX);
        return;
    }
    dest[1] = MP_OBJ_SENTINEL;
}

static const mp_rom_map_elem_t host_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&host_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&hid_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&host_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&host_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_decode), MP_ROM_PTR(&host_decode_obj) },
    { MP_ROM_QSTR(MP_QSTR_char), MP_ROM_PTR(&host_char_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&hid_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&hid_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&hid_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&hid_exit_obj) },
};
static MP_DEFINE_CONST_DICT(host_locals, host_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_hid_host_type,
    MP_QSTR_HIDHost,
    MP_TYPE_FLAG_NONE,
    make_new, host_make_new,
    attr, host_attr,
    locals_dict, &host_locals
    );
