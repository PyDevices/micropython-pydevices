// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.Library: the media library of the phone the sink plays from
// (AVRCP browsing): its players, folders and tracks, the queue and a search.
// It wraps the library half of btclassic.h; mod_btclassic.c registers the
// type. Only BTstack (the Pico W) browses: on the ESP32 the class is there,
// and making one raises.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "btclassic.h"

#if BTC_HAS_LIBRARY

extern const mp_obj_type_t btc_bluetooth_type;

typedef struct {
    mp_obj_base_t base;
} lib_obj_t;

// AVRCP's browsing statuses a phone sends back.
static const char *avrcp_status_text(int status) {
    switch (status) {
        case 0x05: case 0x16: return "the phone's list changed; list it again";
        case 0x08: return "that's not a folder";
        case 0x09: return "no such item; the list may have changed";
        case 0x0c: return "that item can't be played";
        case 0x0d: return "the phone is busy with that item";
        case 0x0f: return "this player can't search";
        case 0x10: return "a search is still running";
        case 0x11: return "no such player";
        case 0x12: return "that player can't be browsed; open its app on the phone first";
        case 0x13: return "that player isn't the one playing";
        case 0x14: return "no results";
        case 0x15: return "the phone has no players";
        default: return NULL;
    }
}

static mp_obj_t lib_done(int err) {
    switch (err) {
        case BTC_OK:
            return mp_const_none;
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: busy, or not connected; connect AudioSink to the phone first"));
        case BTC_ERR_ARG:
            mp_raise_ValueError(NULL);
        default:
            if (err > 0) {
                const char *text = avrcp_status_text(err);
                if (text) {
                    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: %s"), text);
                }
                mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone said 0x%02x"), err);
            }
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the Bluetooth stack refused"));
    }
}

static void lib_check(void) {
    if (!btc_lib_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the library is stopped"));
    }
}

static bool is_utf8(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n;) {
        uint8_t c = s[i];
        size_t more = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 4;
        if (more == 4 || (more && i + more >= n)) {
            return false;
        }
        for (size_t k = 1; k <= more; k++) {
            if ((s[i + k] & 0xc0) != 0x80) {
                return false;
            }
        }
        i += more + 1;
    }
    return true;
}

static mp_obj_t text_from(const uint8_t *s, size_t n) {
    // Phones send UTF-8. A long item comes cut short, maybe inside a
    // character: drop the broken end. Anything else comes back as bytes.
    for (size_t cut = 0; cut <= 3 && cut <= n; cut++) {
        if (is_utf8(s, n - cut)) {
            return mp_obj_new_str((const char *)s, n - cut);
        }
    }
    return mp_obj_new_bytes(s, n);
}

static void put(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// A name: its length, then the text, cut where the item was cut.
static mp_obj_t name_at(const uint8_t *body, size_t n, size_t pos, size_t *after) {
    if (pos + 2 > n) {
        *after = n;
        return MP_OBJ_NEW_QSTR(MP_QSTR_);
    }
    size_t len = be16(body + pos);
    pos += 2;
    if (pos + len > n) {
        len = n - pos;
    }
    *after = pos + len;
    return text_from(body + pos, len);
}

static mp_obj_t parse_item(uint8_t type, const uint8_t *body, size_t n) {
    mp_obj_t d = mp_obj_new_dict(6);
    size_t after;
    switch (type) {
        case 1: {   // a player
            if (n < 28) {
                break;
            }
            put(d, MP_QSTR_id, MP_OBJ_NEW_SMALL_INT(be16(body)));
            put(d, MP_QSTR_name, name_at(body, n, 26, &after));
            put(d, MP_QSTR_playing, mp_obj_new_bool(body[7] == 1));
            put(d, MP_QSTR_browsable, mp_obj_new_bool(body[8 + 7] & 0x08));
            break;
        }
        case 2: {   // a folder
            if (n < 12) {
                break;
            }
            put(d, MP_QSTR_uid, mp_obj_new_bytes(body, 8));
            put(d, MP_QSTR_name, name_at(body, n, 12, &after));
            put(d, MP_QSTR_folder, mp_const_true);
            put(d, MP_QSTR_playable, mp_obj_new_bool(body[9]));
            break;
        }
        case 3: {   // a track or a video
            if (n < 11) {
                break;
            }
            put(d, MP_QSTR_uid, mp_obj_new_bytes(body, 8));
            put(d, MP_QSTR_name, name_at(body, n, 11, &after));
            put(d, MP_QSTR_folder, mp_const_false);
            put(d, MP_QSTR_playable, mp_const_true);
            put(d, MP_QSTR_video, mp_obj_new_bool(body[8] == 1));
            if (after >= n) {
                break;
            }
            size_t pos = after + 1;
            for (int i = 0, count = body[after]; i < count && pos + 8 <= n; i++) {
                uint32_t id = be32(body + pos);
                size_t len = be16(body + pos + 6);
                pos += 8;
                if (pos + len > n) {
                    len = n - pos;
                }
                if (id == 2) {
                    put(d, MP_QSTR_artist, text_from(body + pos, len));
                } else if (id == 3) {
                    put(d, MP_QSTR_album, text_from(body + pos, len));
                }
                pos += len;
            }
            break;
        }
        default:
            break;
    }
    return d;
}

// Wait for the running request; raise on timeout or a refusal.
static void lib_wait(mp_int_t timeout_ms) {
    mp_uint_t start = mp_hal_ticks_ms();
    btc_lib_status_t st;
    for (;;) {
        btc_lib_status(&st);
        if (!st.busy) {
            break;
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout_ms) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(5);
    }
    lib_done(st.result);
}

// The reply's items: dicts, or for a player's path, its folder names.
static mp_obj_t lib_take(bool names) {
    btc_lib_status_t st;
    btc_lib_status(&st);
    uint8_t *buf = m_new(uint8_t, st.length ? st.length : 1);
    size_t n = btc_lib_data(buf, st.length);
    mp_obj_t out = mp_obj_new_list(0, NULL);
    for (size_t pos = 0; pos + 2 <= n;) {
        uint8_t type = buf[pos];
        size_t len = buf[pos + 1];
        pos += 2;
        if (pos + len > n) {
            break;
        }
        if (names) {
            if (type == 4) {
                mp_obj_list_append(out, text_from(buf + pos, len));
            }
        } else if (type >= 1 && type <= 3) {
            mp_obj_list_append(out, parse_item(type, buf + pos, len));
        }
        pos += len;
    }
    m_del(uint8_t, buf, st.length ? st.length : 1);
    return out;
}

static const uint8_t *uid_arg(mp_obj_t obj) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(obj, &buf, MP_BUFFER_READ);
    if (buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("a uid is 8 bytes"));
    }
    return buf.buf;
}

static uint8_t scope_arg(mp_obj_t obj) {
    qstr q = mp_obj_str_get_qstr(obj);
    switch (q) {
        case MP_QSTR_folder: return BTC_LIB_SCOPE_FOLDER;
        case MP_QSTR_search: return BTC_LIB_SCOPE_SEARCH;
        case MP_QSTR_queue: return BTC_LIB_SCOPE_QUEUE;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("scope must be 'folder', 'search' or 'queue'"));
    }
}

// Library(bt, *, max_bytes=16384)
static mp_obj_t lib_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_max_bytes };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 16384} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    if (!btc_radio_active()) {
        lib_done(btc_radio_start());
    }
    if (!btc_lib_started()) {
        mp_int_t max = args[ARG_max_bytes].u_int;
        if (max < 1024) {
            mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be 1024 or more"));
        }
        lib_done(btc_lib_start((size_t)max));
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(lib_obj_t, type));
}

static mp_obj_t lib_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    lib_check();
    btc_lib_changes();
    lib_done(btc_lib_connect());
    mp_int_t timeout = args[ARG_timeout].u_int;
    if (timeout <= 0) {
        return mp_const_none;
    }
    mp_uint_t start = mp_hal_ticks_ms();
    btc_lib_status_t st;
    for (;;) {
        btc_lib_status(&st);
        if (st.connected) {
            return mp_const_none;
        }
        if (!st.connecting) {
            lib_done(st.result ? st.result : BTC_ERR_FAIL);
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(10);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lib_connect_obj, 1, lib_connect);

static mp_obj_t lib_disconnect(mp_obj_t self_in) {
    lib_check();
    lib_done(btc_lib_disconnect());
    mp_uint_t start = mp_hal_ticks_ms();
    btc_lib_status_t st;
    for (btc_lib_status(&st); st.connected && mp_hal_ticks_ms() - start < 3000; btc_lib_status(&st)) {
        mp_event_wait_ms(10);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lib_disconnect_obj, lib_disconnect);

static mp_int_t timeout_kw(mp_map_t *kw_args, mp_int_t def) {
    mp_map_elem_t *e = kw_args ? mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_timeout), MP_MAP_LOOKUP) : NULL;
    return e ? mp_obj_get_int(e->value) : def;
}

// players(*, timeout=5000)
static mp_obj_t lib_players(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    lib_check();
    lib_done(btc_lib_list(BTC_LIB_SCOPE_PLAYERS, 0, 32));
    lib_wait(timeout_kw(kw_args, 5000));
    return lib_take(false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lib_players_obj, 1, lib_players);

// browse(player, *, timeout=5000) -> the folder names down to where it is
static mp_obj_t lib_browse(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    lib_check();
    mp_int_t id = mp_obj_get_int(args[1]);
    if (id < 0 || id > 0xffff) {
        mp_raise_ValueError(MP_ERROR_TEXT("player is an id from players()"));
    }
    lib_done(btc_lib_set_player((uint16_t)id));
    lib_wait(timeout_kw(kw_args, 5000));
    return lib_take(true);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lib_browse_obj, 2, lib_browse);

// items(offset=0, count=20, *, scope="folder", timeout=10000)
static mp_obj_t lib_items(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_offset, ARG_count, ARG_scope, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_offset, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_count, MP_ARG_INT, {.u_int = 20} },
        { MP_QSTR_scope, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_folder)} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    lib_check();
    if (args[ARG_offset].u_int < 0 || args[ARG_count].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset must be 0 or more, and count 1 or more"));
    }
    lib_done(btc_lib_list(scope_arg(args[ARG_scope].u_obj), args[ARG_offset].u_int, args[ARG_count].u_int));
    btc_lib_status_t st;
    mp_uint_t start = mp_hal_ticks_ms();
    for (btc_lib_status(&st); st.busy; btc_lib_status(&st)) {
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= args[ARG_timeout].u_int) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(5);
    }
    // Past the end of a list, or in an empty one, a phone says "range out
    // of bounds": that's an empty page, not a failure.
    if (st.result == 0x0b) {
        return mp_obj_new_list(0, NULL);
    }
    lib_done(st.result);
    return lib_take(false);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lib_items_obj, 1, lib_items);

static mp_obj_t lib_enter(mp_obj_t self_in, mp_obj_t uid) {
    lib_check();
    lib_done(btc_lib_enter(uid_arg(uid)));
    lib_wait(5000);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lib_enter_obj, lib_enter);

static mp_obj_t lib_up(mp_obj_t self_in) {
    lib_check();
    lib_done(btc_lib_up());
    lib_wait(5000);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lib_up_obj, lib_up);

static mp_obj_t lib_search(mp_obj_t self_in, mp_obj_t text) {
    lib_check();
    lib_done(btc_lib_search(mp_obj_str_get_str(text)));
    lib_wait(10000);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lib_search_obj, lib_search);

// play(uid, *, scope="folder")
static mp_obj_t lib_play(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    lib_check();
    mp_map_elem_t *e = kw_args ? mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(MP_QSTR_scope), MP_MAP_LOOKUP) : NULL;
    uint8_t scope = e ? scope_arg(e->value) : BTC_LIB_SCOPE_FOLDER;
    return lib_done(btc_lib_play(scope, uid_arg(args[1])));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lib_play_obj, 2, lib_play);

static mp_obj_t lib_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_lib_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(lib_changes_obj, lib_changes);

static mp_obj_t lib_deinit(mp_obj_t self_in) {
    btc_lib_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lib_deinit_obj, lib_deinit);

static mp_obj_t lib_exit(size_t n_args, const mp_obj_t *args) {
    btc_lib_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lib_exit_obj, 4, 4, lib_exit);

static void lib_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    btc_lib_status_t st;
    btc_lib_status(&st);
    switch (attr) {
        case MP_QSTR_connected:
            dest[0] = mp_obj_new_bool(st.connected);
            return;
        case MP_QSTR_peer:
            dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
            return;
        case MP_QSTR_depth:
            dest[0] = st.depth < 0 ? mp_const_none : MP_OBJ_NEW_SMALL_INT(st.depth);
            return;
        case MP_QSTR_truncated:
            dest[0] = mp_obj_new_bool(st.truncated);
            return;
        default:
            dest[1] = MP_OBJ_SENTINEL;
            return;
    }
}

static const mp_rom_map_elem_t lib_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&lib_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&lib_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_players), MP_ROM_PTR(&lib_players_obj) },
    { MP_ROM_QSTR(MP_QSTR_browse), MP_ROM_PTR(&lib_browse_obj) },
    { MP_ROM_QSTR(MP_QSTR_items), MP_ROM_PTR(&lib_items_obj) },
    { MP_ROM_QSTR(MP_QSTR_enter), MP_ROM_PTR(&lib_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR_up), MP_ROM_PTR(&lib_up_obj) },
    { MP_ROM_QSTR(MP_QSTR_search), MP_ROM_PTR(&lib_search_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&lib_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&lib_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&lib_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&lib_exit_obj) },
};
static MP_DEFINE_CONST_DICT(lib_locals, lib_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_library_type,
    MP_QSTR_Library,
    MP_TYPE_FLAG_NONE,
    make_new, lib_make_new,
    attr, lib_attr,
    locals_dict, &lib_locals
    );

#else

// No browsing in this backend: the class is there so code can check for it,
// and making one says why it can't work.
static mp_obj_t lib_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: Library needs the Pico W; this board's Bluetooth stack can't browse"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    btc_library_type,
    MP_QSTR_Library,
    MP_TYPE_FLAG_NONE,
    make_new, lib_make_new
    );

#endif // BTC_HAS_LIBRARY
