// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio for MicroPython: a Bluetooth speaker's receiving half. A phone or
// computer pairs with the board and plays; Python reads the decoded audio
// into any PCM output, and reads the title, play state and volume, and
// sends play, pause and skip. The work is the btaudio ESP-IDF component in
// ../esp-idf/btaudio; this file only wraps it.

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#if MICROPY_PY_NETWORK
#include "extmod/modnetwork.h"
#endif

#include "btaudio.h"

typedef struct {
    mp_obj_base_t base;
} btaudio_obj_t;

static void btaudio_check(void) {
    if (!btaudio_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btaudio: receiver is closed"));
    }
}

static void btaudio_raise(esp_err_t err) {
    if (err == ESP_ERR_NO_MEM) {
        mp_raise_OSError(MP_ENOMEM);
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btaudio: %s"), esp_err_to_name(err));
}

// Receiver(name=None, *, buffer_ms=500, reconnect=True)
static mp_obj_t btaudio_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_name, ARG_buffer_ms, ARG_reconnect };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_name, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_buffer_ms, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 500} },
        { MP_QSTR_reconnect, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    // One radio, so one receiver. Its state lives in C and outlasts a soft
    // reset: a second Receiver() takes over the running one, link and all.
    if (btaudio_started()) {
        return MP_OBJ_FROM_PTR(mp_obj_malloc(btaudio_obj_t, type));
    }

    const char *name;
    if (args[ARG_name].u_obj != mp_const_none) {
        name = mp_obj_str_get_str(args[ARG_name].u_obj);
    } else {
        #if MICROPY_PY_NETWORK
        // The board's network name, so one board has one name everywhere.
        name = mod_network_hostname_data;
        #else
        name = MICROPY_HW_BOARD_NAME;
        #endif
    }
    mp_int_t ms = args[ARG_buffer_ms].u_int;
    if (ms < 50 || ms > 5000) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer_ms must be 50 to 5000"));
    }
    // Sized for 48 kHz, the fastest rate A2DP's SBC sends.
    esp_err_t err = btaudio_start(name, (size_t)(48 * ms), args[ARG_reconnect].u_bool);
    if (err != ESP_OK) {
        btaudio_raise(err);
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(btaudio_obj_t, type));
}

// readinto(buf) -> frames that came from the stream
static mp_obj_t btaudio_readinto(mp_obj_t self_in, mp_obj_t buf_in) {
    btaudio_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    if (buf.len % 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must hold whole stereo frames (4 bytes each)"));
    }
    size_t n = btaudio_read((int16_t *)buf.buf, buf.len / 4);
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(btaudio_readinto_obj, btaudio_readinto);

static mp_obj_t btaudio_changes_meth(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btaudio_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_changes_obj, btaudio_changes_meth);

static mp_obj_t btaudio_levels_meth(mp_obj_t self_in) {
    uint16_t l, r;
    btaudio_levels(&l, &r);
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(l), MP_OBJ_NEW_SMALL_INT(r) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_levels_obj, btaudio_levels_meth);

static mp_obj_t btaudio_stats(mp_obj_t self_in) {
    btaudio_status_t st;
    btaudio_status(&st);
    mp_obj_t d = mp_obj_new_dict(7);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_buffered), mp_obj_new_int_from_uint(st.buffered));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_capacity), mp_obj_new_int_from_uint(st.capacity));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_received), mp_obj_new_int_from_uint(st.received));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_underruns), mp_obj_new_int_from_uint(st.underruns));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_overruns), mp_obj_new_int_from_uint(st.overruns));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_dropped), mp_obj_new_int_from_uint(st.dropped));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_repeated), mp_obj_new_int_from_uint(st.repeated));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_stats_obj, btaudio_stats);

static mp_obj_t btaudio_send(btaudio_cmd_t cmd) {
    btaudio_check();
    esp_err_t err = btaudio_command(cmd);
    if (err != ESP_OK) {
        btaudio_raise(err);
    }
    return mp_const_none;
}

static mp_obj_t btaudio_play(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PLAY);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_play_obj, btaudio_play);

static mp_obj_t btaudio_pause(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PAUSE);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_pause_obj, btaudio_pause);

static mp_obj_t btaudio_next(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_NEXT);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_next_obj, btaudio_next);

static mp_obj_t btaudio_previous(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PREVIOUS);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_previous_obj, btaudio_previous);

static mp_obj_t btaudio_disconnect_meth(mp_obj_t self_in) {
    btaudio_check();
    esp_err_t err = btaudio_disconnect();
    if (err != ESP_OK) {
        btaudio_raise(err);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_disconnect_obj, btaudio_disconnect_meth);

static mp_obj_t btaudio_close(mp_obj_t self_in) {
    btaudio_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_close_obj, btaudio_close);

static mp_obj_t btaudio_exit(size_t n_args, const mp_obj_t *args) {
    return btaudio_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(btaudio_exit_obj, 4, 4, btaudio_exit);

// Senders send UTF-8, and a field cut to fit can end partway through a
// character: drop that partial character.
static size_t btaudio_utf8_whole(const char *buf, size_t n) {
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

static mp_obj_t btaudio_text_obj(btaudio_text_t which) {
    char buf[128];
    size_t n = btaudio_text(which, buf, sizeof(buf));
    return mp_obj_new_str(buf, btaudio_utf8_whole(buf, n));
}

// Properties: connected, streaming, playing, sample_rate, volume (settable),
// title, artist, album, peer.
static void btaudio_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btaudio_status_t st;
        btaudio_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_streaming:
                dest[0] = mp_obj_new_bool(st.streaming);
                return;
            case MP_QSTR_playing:
                dest[0] = mp_obj_new_bool(st.playing);
                return;
            case MP_QSTR_sample_rate:
                dest[0] = mp_obj_new_int_from_uint(st.sample_rate);
                return;
            case MP_QSTR_volume:
                dest[0] = MP_OBJ_NEW_SMALL_INT(st.volume);
                return;
            case MP_QSTR_title:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_TITLE);
                return;
            case MP_QSTR_artist:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_ARTIST);
                return;
            case MP_QSTR_album:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_ALBUM);
                return;
            case MP_QSTR_peer:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_PEER);
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
        btaudio_set_volume((uint8_t)v);
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t btaudio_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&btaudio_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&btaudio_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_levels), MP_ROM_PTR(&btaudio_levels_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&btaudio_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&btaudio_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&btaudio_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_next), MP_ROM_PTR(&btaudio_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_previous), MP_ROM_PTR(&btaudio_previous_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&btaudio_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&btaudio_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&btaudio_exit_obj) },
};
static MP_DEFINE_CONST_DICT(btaudio_locals, btaudio_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btaudio_type,
    MP_QSTR_Receiver,
    MP_TYPE_FLAG_NONE,
    make_new, btaudio_make_new,
    attr, btaudio_attr,
    locals_dict, &btaudio_locals
    );

static const mp_rom_map_elem_t btaudio_module_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_btaudio) },
    { MP_ROM_QSTR(MP_QSTR_Receiver), MP_ROM_PTR(&btaudio_type) },
    { MP_ROM_QSTR(MP_QSTR_CONNECTION), MP_ROM_INT(BTAUDIO_CHANGED_CONNECTION) },
    { MP_ROM_QSTR(MP_QSTR_STREAM), MP_ROM_INT(BTAUDIO_CHANGED_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_FORMAT), MP_ROM_INT(BTAUDIO_CHANGED_FORMAT) },
    { MP_ROM_QSTR(MP_QSTR_TRACK), MP_ROM_INT(BTAUDIO_CHANGED_TRACK) },
    { MP_ROM_QSTR(MP_QSTR_PLAYING), MP_ROM_INT(BTAUDIO_CHANGED_PLAYING) },
    { MP_ROM_QSTR(MP_QSTR_VOLUME), MP_ROM_INT(BTAUDIO_CHANGED_VOLUME) },
};
static MP_DEFINE_CONST_DICT(btaudio_module_globals, btaudio_module_table);

const mp_obj_module_t btaudio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&btaudio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_btaudio, btaudio_module);
