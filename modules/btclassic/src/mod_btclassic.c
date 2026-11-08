// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic for MicroPython: Bluetooth Classic's radio and profiles. The
// work is a backend behind ../esp-idf/btclassic/include/btclassic.h (on
// ESP32, Bluedroid); this file only wraps it, and talks to nothing else.
//
// Bluetooth is the radio, a singleton like bluetooth.BLE. AudioSink (A2DP
// sink and AVRCP controller) and HandsFree (HFP hands-free unit) take it.
// The backend's state outlives a soft reset: making an object again picks
// up the running one.

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#if MICROPY_PY_NETWORK
#include "extmod/modnetwork.h"
#endif

#include "btclassic.h"

// ---- errors ----

static void raise_btc(int err) {
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

static mp_obj_t done(int err) {
    if (err < 0) {
        raise_btc(err);
    }
    return mp_const_none;
}

// ---- small helpers for building results ----

// A number that may not be known yet: None for BTC_UNKNOWN.
static mp_obj_t maybe_int(int64_t v) {
    return v < 0 ? mp_const_none : mp_obj_new_int_from_ll(v);
}

static mp_obj_t maybe_bool(int v) {
    return v < 0 ? mp_const_none : mp_obj_new_bool(v);
}

static mp_obj_t maybe_str(const char *s) {
    return s ? mp_obj_new_str(s, strlen(s)) : mp_const_none;
}

static mp_obj_t addr_obj(bool valid, const uint8_t *addr) {
    return valid ? mp_obj_new_bytes(addr, 6) : mp_const_none;
}

// An address argument: 6 bytes, or None.
static const uint8_t *addr_arg(mp_obj_t obj) {
    if (obj == mp_const_none) {
        return NULL;
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(obj, &buf, MP_BUFFER_READ);
    if (buf.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
    }
    return buf.buf;
}

static void store(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}

// Senders send UTF-8, and a field cut to fit can end partway through a
// character: drop that partial character.
static size_t utf8_whole(const char *buf, size_t n) {
    if (n == 0) {
        return 0;
    }
    size_t i = n - 1;
    while (i > 0 && ((uint8_t)buf[i] & 0xc0) == 0x80) {
        i--;
    }
    uint8_t lead = (uint8_t)buf[i];
    size_t len = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 : (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
    return i + len > n ? i : n;
}

static mp_obj_t text_obj(char *buf, size_t n) {
    return mp_obj_new_str(buf, utf8_whole(buf, n));
}

static mp_obj_t sink_text(btc_sink_text_t which) {
    char buf[128];
    return text_obj(buf, btc_sink_text(which, buf, sizeof(buf)));
}

static mp_obj_t hf_text(btc_hf_text_t which) {
    char buf[128];
    return text_obj(buf, btc_hf_text(which, buf, sizeof(buf)));
}

static mp_obj_t stats_obj(const btc_ring_stats_t *st) {
    mp_obj_t d = mp_obj_new_dict(8);
    store(d, MP_QSTR_buffered, mp_obj_new_int_from_uint(st->buffered));
    store(d, MP_QSTR_capacity, mp_obj_new_int_from_uint(st->capacity));
    store(d, MP_QSTR_received, mp_obj_new_int_from_uint(st->received));
    store(d, MP_QSTR_underruns, mp_obj_new_int_from_uint(st->underruns));
    store(d, MP_QSTR_overruns, mp_obj_new_int_from_uint(st->overruns));
    store(d, MP_QSTR_dropped, mp_obj_new_int_from_uint(st->dropped));
    store(d, MP_QSTR_repeated, mp_obj_new_int_from_uint(st->repeated));
    store(d, MP_QSTR_skipped, mp_obj_new_int_from_uint(st->skipped));
    return d;
}

static mp_obj_t exit_deinit(size_t n_args, const mp_obj_t *args) {
    mp_obj_t deinit[2];
    mp_load_method(args[0], MP_QSTR_deinit, deinit);
    return mp_call_method_n_kw(0, 0, deinit);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(exit_deinit_obj, 4, 4, exit_deinit);

// =========================================================================
// Bluetooth: the radio
// =========================================================================

typedef struct {
    mp_obj_base_t base;
} btc_obj_t;

extern const mp_obj_type_t btc_bluetooth_type;
static const btc_obj_t bluetooth_singleton = { { &btc_bluetooth_type } };

static mp_obj_t bluetooth_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 0, false);
    char name[2];
    if (btc_radio_name(name, sizeof(name)) == 0) {
        #if MICROPY_PY_NETWORK
        // The board's network name, so one board has one name everywhere.
        btc_radio_set_name(mod_network_hostname_data);
        #else
        btc_radio_set_name(MICROPY_HW_BOARD_NAME);
        #endif
    }
    return MP_OBJ_FROM_PTR(&bluetooth_singleton);
}

static void radio_on(void) {
    if (!btc_radio_active()) {
        done(btc_radio_start());
    }
}

static mp_obj_t bluetooth_active(size_t n_args, const mp_obj_t *args) {
    if (n_args > 1 && args[1] != mp_const_none) {
        if (mp_obj_is_true(args[1])) {
            radio_on();
        } else {
            btc_radio_stop();
        }
    }
    return mp_obj_new_bool(btc_radio_active());
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bluetooth_active_obj, 1, 2, bluetooth_active);

static mp_obj_t bluetooth_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args == 2 && kwargs->used == 0) {
        bool discoverable, connectable;
        btc_radio_visibility(&discoverable, &connectable);
        char buf[249];
        switch (mp_obj_str_get_qstr(args[1])) {
            case MP_QSTR_name:
                return mp_obj_new_str(buf, btc_radio_name(buf, sizeof(buf)));
            case MP_QSTR_pin:
                return mp_obj_new_str(buf, btc_radio_pin(buf, sizeof(buf)));
            case MP_QSTR_discoverable:
                return mp_obj_new_bool(discoverable);
            case MP_QSTR_connectable:
                return mp_obj_new_bool(connectable);
            case MP_QSTR_device_class:
                return mp_obj_new_int_from_uint(btc_radio_class());
            case MP_QSTR_mac: {
                btc_addr_t mac;
                done(btc_radio_mac(mac));
                return mp_obj_new_bytes(mac, 6);
            }
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
        }
    }
    if (n_args != 1) {
        mp_raise_TypeError(MP_ERROR_TEXT("read one setting, or set by keyword"));
    }
    for (size_t i = 0; i < kwargs->alloc; i++) {
        if (!mp_map_slot_is_filled(kwargs, i)) {
            continue;
        }
        mp_obj_t value = kwargs->table[i].value;
        bool discoverable, connectable;
        btc_radio_visibility(&discoverable, &connectable);
        switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
            case MP_QSTR_name:
                done(btc_radio_set_name(mp_obj_str_get_str(value)));
                break;
            case MP_QSTR_pin:
                done(btc_radio_set_pin(mp_obj_str_get_str(value)));
                break;
            case MP_QSTR_discoverable:
                btc_radio_set_visibility(mp_obj_is_true(value), connectable);
                break;
            case MP_QSTR_connectable:
                btc_radio_set_visibility(discoverable, mp_obj_is_true(value));
                break;
            case MP_QSTR_device_class:
                done(btc_radio_set_class(value == mp_const_none ? 0 : mp_obj_get_int(value)));
                break;
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(bluetooth_config_obj, 1, bluetooth_config);

static mp_obj_t bluetooth_paired(mp_obj_t self_in) {
    int n = btc_radio_paired(NULL, 0);
    if (n < 0) {
        raise_btc(n);
    }
    btc_addr_t *addrs = m_new(btc_addr_t, n ? n : 1);
    n = btc_radio_paired(addrs, n);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) {
        mp_obj_list_append(list, mp_obj_new_bytes(addrs[i], 6));
    }
    m_del(btc_addr_t, addrs, n ? n : 1);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bluetooth_paired_obj, bluetooth_paired);

static mp_obj_t bluetooth_unpair(size_t n_args, const mp_obj_t *args) {
    return done(btc_radio_unpair(addr_arg(n_args > 1 ? args[1] : mp_const_none)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bluetooth_unpair_obj, 1, 2, bluetooth_unpair);

// inquiry(duration_ms=10240): look for discoverable devices, forgetting the
// last results; 0 stops one running.
static mp_obj_t bluetooth_inquiry(size_t n_args, const mp_obj_t *args) {
    mp_int_t ms = n_args > 1 ? mp_obj_get_int(args[1]) : 10240;
    if (ms != 0 && (ms < 1280 || ms > 61440)) {
        mp_raise_ValueError(MP_ERROR_TEXT("duration_ms must be 1280 to 61440, or 0 to stop"));
    }
    return done(btc_radio_inquiry((uint32_t)ms));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(bluetooth_inquiry_obj, 1, 2, bluetooth_inquiry);

static mp_obj_t bluetooth_inquiring(mp_obj_t self_in) {
    return mp_obj_new_bool(btc_radio_inquiring());
}
static MP_DEFINE_CONST_FUN_OBJ_1(bluetooth_inquiring_obj, bluetooth_inquiring);

// found() -> [(addr, name, cod, rssi), ...]
static mp_obj_t bluetooth_found(mp_obj_t self_in) {
    btc_found_t *found = m_new(btc_found_t, BTC_FOUND_MAX);
    int n = btc_radio_found(found, BTC_FOUND_MAX);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n && i < BTC_FOUND_MAX; i++) {
        btc_found_t *f = &found[i];
        mp_obj_t t[4] = {
            mp_obj_new_bytes(f->addr, 6),
            text_obj(f->name, strlen(f->name)),
            mp_obj_new_int_from_uint(f->cod),
            f->rssi == BTC_UNKNOWN ? mp_const_none : mp_obj_new_int(f->rssi),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(4, t));
    }
    m_del(btc_found_t, found, BTC_FOUND_MAX);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(bluetooth_found_obj, bluetooth_found);

static mp_obj_t bluetooth_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_radio_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(bluetooth_changes_obj, bluetooth_changes);

static const mp_rom_map_elem_t bluetooth_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&bluetooth_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&bluetooth_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_paired), MP_ROM_PTR(&bluetooth_paired_obj) },
    { MP_ROM_QSTR(MP_QSTR_unpair), MP_ROM_PTR(&bluetooth_unpair_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&bluetooth_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_inquiry), MP_ROM_PTR(&bluetooth_inquiry_obj) },
    { MP_ROM_QSTR(MP_QSTR_inquiring), MP_ROM_PTR(&bluetooth_inquiring_obj) },
    { MP_ROM_QSTR(MP_QSTR_found), MP_ROM_PTR(&bluetooth_found_obj) },
};
static MP_DEFINE_CONST_DICT(bluetooth_locals, bluetooth_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_bluetooth_type,
    MP_QSTR_Bluetooth,
    MP_TYPE_FLAG_NONE,
    make_new, bluetooth_make_new,
    locals_dict, &bluetooth_locals
    );

static void check_radio_arg(mp_obj_t bt) {
    if (!mp_obj_is_type(bt, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    radio_on();
}

// =========================================================================
// AudioSink: A2DP sink and AVRCP controller
// =========================================================================

static void sink_check(void) {
    if (!btc_sink_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the sink is stopped"));
    }
}

static const char *play_status_name(int v) {
    switch (v) {
        case BTC_PLAY_STOPPED:
            return "stopped";
        case BTC_PLAY_PLAYING:
            return "playing";
        case BTC_PLAY_PAUSED:
            return "paused";
        case BTC_PLAY_FWD_SEEK:
            return "seeking forward";
        case BTC_PLAY_REV_SEEK:
            return "seeking back";
        case BTC_PLAY_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static const char *battery_name(int v) {
    static const char *const names[] = { "normal", "warning", "critical", "external", "full" };
    return v >= 0 && v < (int)MP_ARRAY_SIZE(names) ? names[v] : NULL;
}

// The A2DP SBC configuration bits, each one bit set, as numbers and words.
static const char *channel_mode_name(uint8_t bits) {
    return bits & 0x8 ? "mono" : bits & 0x4 ? "dual" : bits & 0x2 ? "stereo" : bits & 0x1 ? "joint stereo" : NULL;
}

static mp_obj_t sbc_pick(uint8_t bits, int v8, int v4, int v2, int v1) {
    int v = bits & 0x8 ? v8 : bits & 0x4 ? v4 : bits & 0x2 ? v2 : bits & 0x1 ? v1 : -1;
    return maybe_int(v);
}

// AudioSink(bt, *, buffer_ms=500, reconnect=True)
static mp_obj_t sink_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_buffer_ms, ARG_reconnect };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_buffer_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 500} },
        { MP_QSTR_reconnect, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    check_radio_arg(args[ARG_bt].u_obj);
    // One sink: a second AudioSink takes over the running one, link and all.
    if (!btc_sink_started()) {
        if (btc_source_started()) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("btclassic: an AudioSource is running; deinit() it first (one A2DP role at a time)"));
        }
        mp_int_t ms = args[ARG_buffer_ms].u_int;
        if (ms < 50 || ms > 5000) {
            mp_raise_ValueError(MP_ERROR_TEXT("buffer_ms must be 50 to 5000"));
        }
        // Sized for 48 kHz, the fastest rate A2DP's SBC sends.
        done(btc_sink_start((size_t)(48 * ms), args[ARG_reconnect].u_bool));
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(btc_obj_t, type));
}

// readinto(buf) -> frames that came from the stream
static mp_obj_t sink_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
    sink_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    if (buf.len % 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must hold whole stereo frames (4 bytes each)"));
    }
    return MP_OBJ_NEW_SMALL_INT(btc_sink_read((int16_t *)buf.buf, buf.len / 4));
}
static MP_DEFINE_CONST_FUN_OBJ_2(sink_readinto_obj, sink_readinto);

static mp_obj_t sink_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_sink_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_changes_obj, sink_changes);

static mp_obj_t sink_levels(mp_obj_t self_in) {
    uint16_t l, r;
    btc_sink_levels(&l, &r);
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(l), MP_OBJ_NEW_SMALL_INT(r) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_levels_obj, sink_levels);

static mp_obj_t sink_stats(mp_obj_t self_in) {
    btc_ring_stats_t st;
    btc_sink_stats(&st);
    return stats_obj(&st);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_stats_obj, sink_stats);

// ---- the player ----

static mp_obj_t sink_press(uint8_t key) {
    sink_check();
    return done(btc_sink_key(key, 2));
}

static mp_obj_t sink_play(mp_obj_t self_in) {
    return sink_press(0x44);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_play_obj, sink_play);

static mp_obj_t sink_stop(mp_obj_t self_in) {
    return sink_press(0x45);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_stop_obj, sink_stop);

static mp_obj_t sink_pause(mp_obj_t self_in) {
    return sink_press(0x46);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_pause_obj, sink_pause);

static mp_obj_t sink_next(mp_obj_t self_in) {
    return sink_press(0x4b);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_next_obj, sink_next);

static mp_obj_t sink_previous(mp_obj_t self_in) {
    return sink_press(0x4c);
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_previous_obj, sink_previous);

// seek(direction): 1 starts fast forward, -1 starts rewind, 0 stops either.
static mp_obj_t sink_seek(mp_obj_t self_in, mp_obj_t dir_in) {
    sink_check();
    static int8_t held;     // the key being held: 1 forward, -1 back
    mp_int_t dir = mp_obj_get_int(dir_in);
    int err = BTC_OK;
    if (held) {
        err = btc_sink_key(held > 0 ? 0x49 : 0x48, 1);
        held = 0;
    }
    if (err == BTC_OK && dir) {
        err = btc_sink_key(dir > 0 ? 0x49 : 0x48, 0);
        if (err == BTC_OK) {
            held = dir > 0 ? 1 : -1;
        }
    }
    return done(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sink_seek_obj, sink_seek);

// key(code, state=2): any AVRCP pass-through key; 0 press, 1 release, 2 both.
static mp_obj_t sink_key(size_t n_args, const mp_obj_t *args) {
    sink_check();
    mp_int_t code = mp_obj_get_int(args[1]);
    mp_int_t state = n_args > 2 ? mp_obj_get_int(args[2]) : 2;
    if (code < 0 || code > 0x7f || state < 0 || state > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("key code 0 to 127, state 0 to 2"));
    }
    return done(btc_sink_key((uint8_t)code, (int)state));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sink_key_obj, 2, 3, sink_key);

// setting(name, value): the sender's player settings. Senders often ignore
// these; AVRCP gives no way to read them back.
static mp_obj_t sink_setting(mp_obj_t self_in, mp_obj_t name_in, mp_obj_t value_in) {
    sink_check();
    qstr name = mp_obj_str_get_qstr(name_in);
    qstr value = mp_obj_str_get_qstr(value_in);
    uint8_t attr = 0, v = 0;
    switch (name) {
        case MP_QSTR_equalizer:
            attr = 1;
            v = value == MP_QSTR_off ? 1 : value == MP_QSTR_on ? 2 : 0;
            break;
        case MP_QSTR_repeat:
            attr = 2;
            v = value == MP_QSTR_off ? 1 : value == MP_QSTR_single ? 2 : value == MP_QSTR_all ? 3 : 0;
            break;
        case MP_QSTR_shuffle:
        case MP_QSTR_scan:
            attr = name == MP_QSTR_shuffle ? 3 : 4;
            v = value == MP_QSTR_off ? 1 : value == MP_QSTR_all ? 2 : value == MP_QSTR_group ? 3 : 0;
            break;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("setting is equalizer, repeat, shuffle or scan"));
    }
    if (v == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("equalizer: off/on; repeat: off/single/all; shuffle, scan: off/all/group"));
    }
    return done(btc_sink_set_player(attr, v));
}
static MP_DEFINE_CONST_FUN_OBJ_3(sink_setting_obj, sink_setting);

static mp_obj_t sink_refresh(mp_obj_t self_in) {
    sink_check();
    return done(btc_sink_refresh());
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_refresh_obj, sink_refresh);

static mp_obj_t sink_link(mp_obj_t self_in) {
    btc_sink_status_t st;
    btc_sink_status(&st);
    mp_obj_t d = mp_obj_new_dict(17);
    store(d, MP_QSTR_address, addr_obj(st.connected, st.peer));
    store(d, MP_QSTR_name, sink_text(BTC_SINK_PEER_NAME));
    store(d, MP_QSTR_rssi_delta, st.connected && st.rssi_delta != BTC_UNKNOWN ? mp_obj_new_int(st.rssi_delta) : mp_const_none);
    store(d, MP_QSTR_codec, st.sample_rate ? MP_OBJ_NEW_QSTR(MP_QSTR_SBC) : mp_const_none);
    store(d, MP_QSTR_sample_rate, mp_obj_new_int_from_uint(st.sample_rate));
    store(d, MP_QSTR_channels, MP_OBJ_NEW_SMALL_INT(st.channels));
    store(d, MP_QSTR_channel_mode, maybe_str(channel_mode_name(st.sbc_channel_mode)));
    store(d, MP_QSTR_block_length, sbc_pick(st.sbc_block_length, 4, 8, 12, 16));
    store(d, MP_QSTR_subbands, sbc_pick(st.sbc_subbands, -1, -1, 4, 8));
    store(d, MP_QSTR_allocation, maybe_str(st.sbc_allocation & 0x2 ? "snr" : st.sbc_allocation & 0x1 ? "loudness" : NULL));
    store(d, MP_QSTR_bitpool_min, MP_OBJ_NEW_SMALL_INT(st.sbc_min_bitpool));
    store(d, MP_QSTR_bitpool_max, MP_OBJ_NEW_SMALL_INT(st.sbc_max_bitpool));
    store(d, MP_QSTR_delay_ms, MP_OBJ_NEW_SMALL_INT(st.delay_ms));
    store(d, MP_QSTR_avrcp_features, mp_obj_new_int_from_uint(st.avrcp_features));
    store(d, MP_QSTR_avrcp_target_flags, MP_OBJ_NEW_SMALL_INT(st.avrcp_tg_flags));
    store(d, MP_QSTR_cover_art, mp_obj_new_bool(st.cover_art));
    store(d, MP_QSTR_battery, maybe_str(battery_name(st.battery)));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_link_obj, sink_link);

static mp_obj_t sink_connect(size_t n_args, const mp_obj_t *args) {
    sink_check();
    return done(btc_sink_connect(addr_arg(n_args > 1 ? args[1] : mp_const_none)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(sink_connect_obj, 1, 2, sink_connect);

static mp_obj_t sink_disconnect(mp_obj_t self_in) {
    sink_check();
    return done(btc_sink_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_disconnect_obj, sink_disconnect);

static mp_obj_t sink_deinit(mp_obj_t self_in) {
    btc_sink_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(sink_deinit_obj, sink_deinit);

static mp_obj_t sink_cover(void) {
    // The image can change between asking its size and copying it: retry.
    for (int tries = 0; tries < 3; tries++) {
        size_t size = btc_sink_cover_size();
        if (size == 0) {
            return mp_const_none;
        }
        vstr_t v;
        vstr_init_len(&v, size);
        size_t n = btc_sink_cover((uint8_t *)v.buf, size);
        if (n == size) {
            return mp_obj_new_bytes_from_vstr(&v);
        }
        vstr_clear(&v);
    }
    return mp_const_none;
}

// cover_into(buf) -> bytes: the cover copied into a buffer the program keeps,
// so a new cover makes no garbage.
static mp_obj_t sink_cover_into(mp_obj_t self_in, mp_obj_t buf_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    for (int tries = 0; tries < 3; tries++) {
        size_t size = btc_sink_cover_size();
        if (size == 0) {
            return MP_OBJ_NEW_SMALL_INT(0);
        }
        if (size > buf.len) {
            mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("the cover is %u bytes"), (unsigned)size);
        }
        size_t n = btc_sink_cover((uint8_t *)buf.buf, buf.len);
        if (n) {
            return MP_OBJ_NEW_SMALL_INT(n);
        }
    }
    return MP_OBJ_NEW_SMALL_INT(0);
}
static MP_DEFINE_CONST_FUN_OBJ_2(sink_cover_into_obj, sink_cover_into);

static void sink_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_sink_status_t st;
        btc_sink_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_streaming:
                dest[0] = mp_obj_new_bool(st.streaming);
                return;
            case MP_QSTR_playing:
                dest[0] = mp_obj_new_bool(st.play_status == BTC_PLAY_PLAYING);
                return;
            case MP_QSTR_status:
                dest[0] = maybe_str(play_status_name(st.play_status));
                return;
            case MP_QSTR_sample_rate:
                dest[0] = mp_obj_new_int_from_uint(st.sample_rate);
                return;
            case MP_QSTR_volume:
                dest[0] = MP_OBJ_NEW_SMALL_INT(st.volume);
                return;
            case MP_QSTR_title:
                dest[0] = sink_text(BTC_SINK_TITLE);
                return;
            case MP_QSTR_artist:
                dest[0] = sink_text(BTC_SINK_ARTIST);
                return;
            case MP_QSTR_album:
                dest[0] = sink_text(BTC_SINK_ALBUM);
                return;
            case MP_QSTR_genre:
                dest[0] = sink_text(BTC_SINK_GENRE);
                return;
            case MP_QSTR_track_number:
                dest[0] = maybe_int(st.track_number);
                return;
            case MP_QSTR_track_count:
                dest[0] = maybe_int(st.track_count);
                return;
            case MP_QSTR_duration:
                dest[0] = maybe_int(st.duration_ms);
                return;
            case MP_QSTR_position:
                dest[0] = maybe_int(st.position_ms);
                return;
            case MP_QSTR_cover:
                dest[0] = sink_cover();
                return;
            case MP_QSTR_cover_size:
                dest[0] = MP_OBJ_NEW_SMALL_INT(btc_sink_cover_size());
                return;
            case MP_QSTR_peer:
                dest[0] = addr_obj(st.connected, st.peer);
                return;
            case MP_QSTR_peer_name:
                dest[0] = sink_text(BTC_SINK_PEER_NAME);
                return;
            default:
                dest[1] = MP_OBJ_SENTINEL;  // methods, from the locals dict
                return;
        }
    } else if (dest[1] != MP_OBJ_NULL && attr == MP_QSTR_volume) {
        mp_int_t v = mp_obj_get_int(dest[1]);
        if (v < 0 || v > 127) {
            mp_raise_ValueError(MP_ERROR_TEXT("volume must be 0 to 127"));
        }
        btc_sink_set_volume((uint8_t)v);
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t sink_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&sink_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&sink_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_levels), MP_ROM_PTR(&sink_levels_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&sink_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&sink_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&sink_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&sink_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_next), MP_ROM_PTR(&sink_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_previous), MP_ROM_PTR(&sink_previous_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&sink_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&sink_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_setting), MP_ROM_PTR(&sink_setting_obj) },
    { MP_ROM_QSTR(MP_QSTR_refresh), MP_ROM_PTR(&sink_refresh_obj) },
    { MP_ROM_QSTR(MP_QSTR_link), MP_ROM_PTR(&sink_link_obj) },
    { MP_ROM_QSTR(MP_QSTR_cover_into), MP_ROM_PTR(&sink_cover_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&sink_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&sink_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&sink_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&exit_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(sink_locals, sink_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_sink_type,
    MP_QSTR_AudioSink,
    MP_TYPE_FLAG_NONE,
    make_new, sink_make_new,
    attr, sink_attr,
    locals_dict, &sink_locals
    );

// =========================================================================
// HandsFree: HFP hands-free unit
// =========================================================================

static void hf_check(void) {
    if (!btc_hf_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the hands-free unit is stopped"));
    }
}

static const char *call_name(int v) {
    static const char *const names[] = { "idle", "incoming", "dialing", "alerting", "active", "held" };
    return v >= 0 && v < (int)MP_ARRAY_SIZE(names) ? names[v] : NULL;
}

static uint8_t hf_channels;     // the frame shape readinto fills

// HandsFree(bt, *, buffer_ms=400, channels=1)
static mp_obj_t hf_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_buffer_ms, ARG_channels };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_buffer_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 400} },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    check_radio_arg(args[ARG_bt].u_obj);
    if (!btc_hf_started()) {
        mp_int_t ms = args[ARG_buffer_ms].u_int;
        mp_int_t channels = args[ARG_channels].u_int;
        if (ms < 50 || ms > 1000) {
            mp_raise_ValueError(MP_ERROR_TEXT("buffer_ms must be 50 to 1000"));
        }
        if (channels != 1 && channels != 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("channels must be 1 or 2"));
        }
        done(btc_hf_start((uint32_t)ms, (uint8_t)channels));
        hf_channels = (uint8_t)channels;
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(btc_obj_t, type));
}

static mp_obj_t hf_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
    hf_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    size_t width = 2 * (hf_channels ? hf_channels : 1);
    if (buf.len % width) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must hold whole frames"));
    }
    return MP_OBJ_NEW_SMALL_INT(btc_hf_read((int16_t *)buf.buf, buf.len / width));
}
static MP_DEFINE_CONST_FUN_OBJ_2(hf_readinto_obj, hf_readinto);

// write(buf) -> bytes taken: the microphone, to the phone
static mp_obj_t hf_write(mp_obj_t self_in, mp_obj_t buf_in) {
    hf_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    return MP_OBJ_NEW_SMALL_INT(btc_hf_write((const int16_t *)buf.buf, buf.len / 2) * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_2(hf_write_obj, hf_write);

static mp_obj_t hf_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_hf_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_changes_obj, hf_changes);

static mp_obj_t hf_stats(mp_obj_t self_in) {
    btc_ring_stats_t st;
    btc_hf_stats(&st);
    return stats_obj(&st);
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_stats_obj, hf_stats);

static mp_obj_t hf_answer(mp_obj_t self_in) {
    hf_check();
    return done(btc_hf_answer());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_answer_obj, hf_answer);

static mp_obj_t hf_hangup(mp_obj_t self_in) {
    hf_check();
    return done(btc_hf_hangup());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_hangup_obj, hf_hangup);

// dial(number=None): None redials the last number.
static mp_obj_t hf_dial(size_t n_args, const mp_obj_t *args) {
    hf_check();
    const char *number = n_args > 1 && args[1] != mp_const_none ? mp_obj_str_get_str(args[1]) : NULL;
    return done(btc_hf_dial(number));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hf_dial_obj, 1, 2, hf_dial);

static mp_obj_t hf_dtmf(mp_obj_t self_in, mp_obj_t digits_in) {
    hf_check();
    size_t n;
    const char *digits = mp_obj_str_get_data(digits_in, &n);
    for (size_t i = 0; i < n; i++) {
        done(btc_hf_dtmf(digits[i]));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(hf_dtmf_obj, hf_dtmf);

static mp_obj_t hf_connect_audio(mp_obj_t self_in) {
    hf_check();
    return done(btc_hf_connect_audio());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_connect_audio_obj, hf_connect_audio);

static mp_obj_t hf_disconnect_audio(mp_obj_t self_in) {
    hf_check();
    return done(btc_hf_disconnect_audio());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_disconnect_audio_obj, hf_disconnect_audio);

static mp_obj_t hf_connect(size_t n_args, const mp_obj_t *args) {
    hf_check();
    return done(btc_hf_connect(addr_arg(n_args > 1 ? args[1] : mp_const_none)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(hf_connect_obj, 1, 2, hf_connect);

static mp_obj_t hf_disconnect(mp_obj_t self_in) {
    hf_check();
    return done(btc_hf_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_disconnect_obj, hf_disconnect);

static mp_obj_t hf_deinit(mp_obj_t self_in) {
    btc_hf_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(hf_deinit_obj, hf_deinit);

static mp_obj_t gain_arg(mp_obj_t v, int (*set)(int)) {
    mp_int_t g = mp_obj_get_int(v);
    if (g < 0 || g > 15) {
        mp_raise_ValueError(MP_ERROR_TEXT("gain must be 0 to 15"));
    }
    return done(set((int)g));
}

static void hf_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_hf_status_t st;
        btc_hf_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_peer:
                dest[0] = addr_obj(st.connected, st.peer);
                return;
            case MP_QSTR_service:
                dest[0] = maybe_bool(st.service);
                return;
            case MP_QSTR_roaming:
                dest[0] = maybe_bool(st.roaming);
                return;
            case MP_QSTR_signal:
                dest[0] = maybe_int(st.signal);
                return;
            case MP_QSTR_battery:
                dest[0] = maybe_int(st.battery);
                return;
            case MP_QSTR_operator:
                dest[0] = hf_text(BTC_HF_OPERATOR);
                return;
            case MP_QSTR_number:
                dest[0] = hf_text(BTC_HF_SUBSCRIBER);
                return;
            case MP_QSTR_call:
                dest[0] = st.connected ? maybe_str(call_name(st.call)) : mp_const_none;
                return;
            case MP_QSTR_caller:
                dest[0] = hf_text(BTC_HF_CALLER);
                return;
            case MP_QSTR_held:
                dest[0] = MP_OBJ_NEW_SMALL_INT(st.held);
                return;
            case MP_QSTR_waiting:
                dest[0] = mp_obj_new_bool(st.waiting);
                return;
            case MP_QSTR_ringing:
                dest[0] = mp_obj_new_bool(st.ringing);
                return;
            case MP_QSTR_in_band_ring:
                dest[0] = mp_obj_new_bool(st.in_band_ring);
                return;
            case MP_QSTR_voice_recognition:
                dest[0] = mp_obj_new_bool(st.voice_recognition);
                return;
            case MP_QSTR_speaker_gain:
                dest[0] = maybe_int(st.speaker_gain);
                return;
            case MP_QSTR_mic_gain:
                dest[0] = maybe_int(st.mic_gain);
                return;
            case MP_QSTR_audio:
                dest[0] = mp_obj_new_bool(st.audio);
                return;
            case MP_QSTR_sample_rate:
                dest[0] = mp_obj_new_int_from_uint(st.sample_rate);
                return;
            default:
                dest[1] = MP_OBJ_SENTINEL;  // methods, from the locals dict
                return;
        }
    } else if (dest[1] != MP_OBJ_NULL) {
        switch (attr) {
            case MP_QSTR_voice_recognition:
                hf_check();
                done(btc_hf_voice_recognition(mp_obj_is_true(dest[1])));
                break;
            case MP_QSTR_speaker_gain:
                hf_check();
                gain_arg(dest[1], btc_hf_set_speaker_gain);
                break;
            case MP_QSTR_mic_gain:
                hf_check();
                gain_arg(dest[1], btc_hf_set_mic_gain);
                break;
            default:
                return;     // read only: AttributeError
        }
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t hf_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&hf_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&hf_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&hf_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&hf_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_answer), MP_ROM_PTR(&hf_answer_obj) },
    { MP_ROM_QSTR(MP_QSTR_hangup), MP_ROM_PTR(&hf_hangup_obj) },
    { MP_ROM_QSTR(MP_QSTR_dial), MP_ROM_PTR(&hf_dial_obj) },
    { MP_ROM_QSTR(MP_QSTR_dtmf), MP_ROM_PTR(&hf_dtmf_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect_audio), MP_ROM_PTR(&hf_connect_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect_audio), MP_ROM_PTR(&hf_disconnect_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&hf_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&hf_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&hf_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&exit_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(hf_locals, hf_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_hf_type,
    MP_QSTR_HandsFree,
    MP_TYPE_FLAG_NONE,
    make_new, hf_make_new,
    attr, hf_attr,
    locals_dict, &hf_locals
    );

// AudioSource, in mod_btclassic_source.c.
extern const mp_obj_type_t btc_source_type;
// Serial, in mod_btclassic_serial.c.
extern const mp_obj_type_t btc_serial_type;

// HIDDevice and HIDHost, in mod_btclassic_hid.c, where the backend has HID.
#if BTC_HAS_HID
extern const mp_obj_type_t btc_hid_device_type;
extern const mp_obj_type_t btc_hid_host_type;
#endif

// PhoneBook and AudioGateway, in mod_btclassic_phone.c.
extern const mp_obj_type_t btc_phonebook_type;
extern const mp_obj_type_t btc_gateway_type;

// =========================================================================
// the module
// =========================================================================

static const mp_rom_map_elem_t btclassic_module_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_btclassic) },
    { MP_ROM_QSTR(MP_QSTR_Bluetooth), MP_ROM_PTR(&btc_bluetooth_type) },
    { MP_ROM_QSTR(MP_QSTR_AudioSink), MP_ROM_PTR(&btc_sink_type) },
    { MP_ROM_QSTR(MP_QSTR_HandsFree), MP_ROM_PTR(&btc_hf_type) },
    { MP_ROM_QSTR(MP_QSTR_AudioSource), MP_ROM_PTR(&btc_source_type) },
    { MP_ROM_QSTR(MP_QSTR_Serial), MP_ROM_PTR(&btc_serial_type) },
    { MP_ROM_QSTR(MP_QSTR_PhoneBook), MP_ROM_PTR(&btc_phonebook_type) },
    { MP_ROM_QSTR(MP_QSTR_AudioGateway), MP_ROM_PTR(&btc_gateway_type) },
    { MP_ROM_QSTR(MP_QSTR_PHONEBOOK), MP_ROM_INT(BTC_CHANGED_PHONEBOOK) },
    { MP_ROM_QSTR(MP_QSTR_REQUEST), MP_ROM_INT(BTC_CHANGED_REQUEST) },
    #if BTC_HAS_HID
    { MP_ROM_QSTR(MP_QSTR_HIDDevice), MP_ROM_PTR(&btc_hid_device_type) },
    { MP_ROM_QSTR(MP_QSTR_HIDHost), MP_ROM_PTR(&btc_hid_host_type) },
    { MP_ROM_QSTR(MP_QSTR_HID), MP_ROM_INT(BTC_CHANGED_HID) },
    // HIDDevice subclasses: what kind of device the host sees.
    { MP_ROM_QSTR(MP_QSTR_HID_KEYBOARD), MP_ROM_INT(0x40) },
    { MP_ROM_QSTR(MP_QSTR_HID_MOUSE), MP_ROM_INT(0x80) },
    { MP_ROM_QSTR(MP_QSTR_HID_GAMEPAD), MP_ROM_INT(0x08) },
    #endif
    { MP_ROM_QSTR(MP_QSTR_CONNECTION), MP_ROM_INT(BTC_CHANGED_CONNECTION) },
    { MP_ROM_QSTR(MP_QSTR_STREAM), MP_ROM_INT(BTC_CHANGED_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_FORMAT), MP_ROM_INT(BTC_CHANGED_FORMAT) },
    { MP_ROM_QSTR(MP_QSTR_TRACK), MP_ROM_INT(BTC_CHANGED_TRACK) },
    { MP_ROM_QSTR(MP_QSTR_PLAYING), MP_ROM_INT(BTC_CHANGED_PLAYING) },
    { MP_ROM_QSTR(MP_QSTR_VOLUME), MP_ROM_INT(BTC_CHANGED_VOLUME) },
    { MP_ROM_QSTR(MP_QSTR_POSITION), MP_ROM_INT(BTC_CHANGED_POSITION) },
    { MP_ROM_QSTR(MP_QSTR_PEER), MP_ROM_INT(BTC_CHANGED_PEER) },
    { MP_ROM_QSTR(MP_QSTR_COVER), MP_ROM_INT(BTC_CHANGED_COVER) },
    { MP_ROM_QSTR(MP_QSTR_PHONE), MP_ROM_INT(BTC_CHANGED_PHONE) },
    { MP_ROM_QSTR(MP_QSTR_CALL), MP_ROM_INT(BTC_CHANGED_CALL) },
    { MP_ROM_QSTR(MP_QSTR_AUDIO), MP_ROM_INT(BTC_CHANGED_AUDIO) },
    { MP_ROM_QSTR(MP_QSTR_PAIRED), MP_ROM_INT(BTC_CHANGED_PAIRED) },
    { MP_ROM_QSTR(MP_QSTR_REMOTE), MP_ROM_INT(BTC_CHANGED_REMOTE) },
    { MP_ROM_QSTR(MP_QSTR_FOUND), MP_ROM_INT(BTC_CHANGED_FOUND) },
    { MP_ROM_QSTR(MP_QSTR_SERIAL), MP_ROM_INT(BTC_CHANGED_SERIAL) },
    // AVRCP pass-through key codes: AudioSink.key() sends them, and
    // AudioSource.key() reports the remote's.
    { MP_ROM_QSTR(MP_QSTR_KEY_PLAY), MP_ROM_INT(0x44) },
    { MP_ROM_QSTR(MP_QSTR_KEY_STOP), MP_ROM_INT(0x45) },
    { MP_ROM_QSTR(MP_QSTR_KEY_PAUSE), MP_ROM_INT(0x46) },
    { MP_ROM_QSTR(MP_QSTR_KEY_NEXT), MP_ROM_INT(0x4b) },
    { MP_ROM_QSTR(MP_QSTR_KEY_PREVIOUS), MP_ROM_INT(0x4c) },
    { MP_ROM_QSTR(MP_QSTR_KEY_VOLUME_UP), MP_ROM_INT(0x41) },
    { MP_ROM_QSTR(MP_QSTR_KEY_VOLUME_DOWN), MP_ROM_INT(0x42) },
    { MP_ROM_QSTR(MP_QSTR_KEY_MUTE), MP_ROM_INT(0x43) },
    { MP_ROM_QSTR(MP_QSTR_KEY_REWIND), MP_ROM_INT(0x48) },
    { MP_ROM_QSTR(MP_QSTR_KEY_FAST_FORWARD), MP_ROM_INT(0x49) },
};
static MP_DEFINE_CONST_DICT(btclassic_module_globals, btclassic_module_table);

const mp_obj_module_t btclassic_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&btclassic_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_btclassic, btclassic_module);
