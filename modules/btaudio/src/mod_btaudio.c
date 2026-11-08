// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio for MicroPython: a Bluetooth speaker's receiving half. A phone or
// computer pairs with the board and plays; Python reads the decoded audio
// into any PCM output, everything the sender says about the track (cover art
// included), its play status and position, and the phone's own battery,
// signal, carrier and calls; and drives the player and the calls. The work
// is the btaudio ESP-IDF component in ../esp-idf/btaudio; this file only
// wraps it.

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
    if (err == ESP_ERR_NOT_SUPPORTED) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btaudio: this firmware has no hands-free (HFP)"));
    }
    if (err == ESP_ERR_INVALID_STATE) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btaudio: not connected"));
    }
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btaudio: %s"), esp_err_to_name(err));
}

static mp_obj_t btaudio_done(esp_err_t err) {
    if (err != ESP_OK) {
        btaudio_raise(err);
    }
    return mp_const_none;
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

// ---- small helpers for building results ----

// A number that may not be known yet: None for BTAUDIO_UNKNOWN.
static mp_obj_t maybe_int(int64_t v) {
    return v < 0 ? mp_const_none : mp_obj_new_int_from_ll(v);
}

static mp_obj_t maybe_str(const char *s) {
    return s ? mp_obj_new_str(s, strlen(s)) : mp_const_none;
}

static void store(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}

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

static const char *play_status_name(int v) {
    switch (v) {
        case BTAUDIO_PLAY_STOPPED:
            return "stopped";
        case BTAUDIO_PLAY_PLAYING:
            return "playing";
        case BTAUDIO_PLAY_PAUSED:
            return "paused";
        case BTAUDIO_PLAY_FWD_SEEK:
            return "seeking forward";
        case BTAUDIO_PLAY_REV_SEEK:
            return "seeking back";
        case BTAUDIO_PLAY_ERROR:
            return "error";
        default:
            return NULL;
    }
}

static const char *call_name(int v) {
    static const char *const names[] = { "idle", "incoming", "dialing", "alerting", "active", "held" };
    return v >= 0 && v < (int)MP_ARRAY_SIZE(names) ? names[v] : NULL;
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

// ---- audio ----

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
    store(d, MP_QSTR_buffered, mp_obj_new_int_from_uint(st.buffered));
    store(d, MP_QSTR_capacity, mp_obj_new_int_from_uint(st.capacity));
    store(d, MP_QSTR_received, mp_obj_new_int_from_uint(st.received));
    store(d, MP_QSTR_underruns, mp_obj_new_int_from_uint(st.underruns));
    store(d, MP_QSTR_overruns, mp_obj_new_int_from_uint(st.overruns));
    store(d, MP_QSTR_dropped, mp_obj_new_int_from_uint(st.dropped));
    store(d, MP_QSTR_repeated, mp_obj_new_int_from_uint(st.repeated));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_stats_obj, btaudio_stats);

// ---- the player ----

static mp_obj_t btaudio_send(btaudio_cmd_t cmd) {
    btaudio_check();
    return btaudio_done(btaudio_command(cmd));
}

static mp_obj_t btaudio_play(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PLAY);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_play_obj, btaudio_play);

static mp_obj_t btaudio_pause(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PAUSE);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_pause_obj, btaudio_pause);

static mp_obj_t btaudio_stop_meth(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_STOP);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_stop_obj, btaudio_stop_meth);

static mp_obj_t btaudio_next(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_NEXT);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_next_obj, btaudio_next);

static mp_obj_t btaudio_previous(mp_obj_t self_in) {
    return btaudio_send(BTAUDIO_CMD_PREVIOUS);
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_previous_obj, btaudio_previous);

// seek(direction): 1 starts fast forward, -1 starts rewind, 0 stops either.
static mp_obj_t btaudio_seek(mp_obj_t self_in, mp_obj_t dir_in) {
    btaudio_check();
    static int8_t held;     // the key being held: 1 forward, -1 back
    mp_int_t dir = mp_obj_get_int(dir_in);
    esp_err_t err = ESP_OK;
    if (held) {
        err = btaudio_key(held > 0 ? 0x49 : 0x48, 1);
        held = 0;
    }
    if (err == ESP_OK && dir) {
        err = btaudio_key(dir > 0 ? 0x49 : 0x48, 0);
        if (err == ESP_OK) {
            held = dir > 0 ? 1 : -1;
        }
    }
    return btaudio_done(err);
}
static MP_DEFINE_CONST_FUN_OBJ_2(btaudio_seek_obj, btaudio_seek);

// key(code, state=2): any AVRCP pass-through key; 0 press, 1 release, 2 both.
static mp_obj_t btaudio_key_meth(size_t n_args, const mp_obj_t *args) {
    btaudio_check();
    mp_int_t code = mp_obj_get_int(args[1]);
    mp_int_t state = n_args > 2 ? mp_obj_get_int(args[2]) : 2;
    if (code < 0 || code > 0x7f || state < 0 || state > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("key code 0 to 127, state 0 to 2"));
    }
    return btaudio_done(btaudio_key((uint8_t)code, (int)state));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(btaudio_key_obj, 2, 3, btaudio_key_meth);

// setting(name, value): the sender's player settings. Senders often ignore
// these; AVRCP gives no way to read them back.
static mp_obj_t btaudio_setting(mp_obj_t self_in, mp_obj_t name_in, mp_obj_t value_in) {
    btaudio_check();
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
    return btaudio_done(btaudio_set_player(attr, v));
}
static MP_DEFINE_CONST_FUN_OBJ_3(btaudio_setting_obj, btaudio_setting);

static mp_obj_t btaudio_refresh_meth(mp_obj_t self_in) {
    btaudio_check();
    return btaudio_done(btaudio_refresh());
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_refresh_obj, btaudio_refresh_meth);

// ---- the link and the phone, as dicts ----

static mp_obj_t btaudio_link(mp_obj_t self_in) {
    btaudio_status_t st;
    btaudio_status(&st);
    mp_obj_t d = mp_obj_new_dict(18);
    store(d, MP_QSTR_address, btaudio_text_obj(BTAUDIO_TEXT_PEER));
    store(d, MP_QSTR_name, btaudio_text_obj(BTAUDIO_TEXT_PEER_NAME));
    store(d, MP_QSTR_rssi_delta, st.connected && st.rssi_delta != BTAUDIO_UNKNOWN ? mp_obj_new_int(st.rssi_delta) : mp_const_none);
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
    store(d, MP_QSTR_hands_free, mp_obj_new_bool(st.hf_connected));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_link_obj, btaudio_link);

static mp_obj_t btaudio_phone(mp_obj_t self_in) {
    btaudio_status_t st;
    btaudio_status(&st);
    mp_obj_t d = mp_obj_new_dict(16);
    store(d, MP_QSTR_connected, mp_obj_new_bool(st.hf_connected));
    store(d, MP_QSTR_service, st.service < 0 ? mp_const_none : mp_obj_new_bool(st.service));
    store(d, MP_QSTR_signal, maybe_int(st.signal));
    store(d, MP_QSTR_roaming, st.roaming < 0 ? mp_const_none : mp_obj_new_bool(st.roaming));
    store(d, MP_QSTR_battery, maybe_int(st.phone_battery));
    store(d, MP_QSTR_operator, btaudio_text_obj(BTAUDIO_TEXT_OPERATOR));
    store(d, MP_QSTR_number, btaudio_text_obj(BTAUDIO_TEXT_SUBSCRIBER));
    store(d, MP_QSTR_call, maybe_str(call_name(st.call)));
    store(d, MP_QSTR_caller, btaudio_text_obj(BTAUDIO_TEXT_CALLER));
    store(d, MP_QSTR_held, MP_OBJ_NEW_SMALL_INT(st.held));
    store(d, MP_QSTR_waiting, mp_obj_new_bool(st.waiting));
    store(d, MP_QSTR_ringing, mp_obj_new_bool(st.ringing));
    store(d, MP_QSTR_voice, mp_obj_new_bool(st.voice));
    store(d, MP_QSTR_in_band_ring, mp_obj_new_bool(st.in_band_ring));
    store(d, MP_QSTR_speaker_gain, maybe_int(st.speaker_gain));
    store(d, MP_QSTR_mic_gain, maybe_int(st.mic_gain));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_phone_obj, btaudio_phone);

// ---- calls ----

static mp_obj_t btaudio_answer_meth(mp_obj_t self_in) {
    btaudio_check();
    return btaudio_done(btaudio_answer());
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_answer_obj, btaudio_answer_meth);

static mp_obj_t btaudio_hangup_meth(mp_obj_t self_in) {
    btaudio_check();
    return btaudio_done(btaudio_hangup());
}
static MP_DEFINE_CONST_FUN_OBJ_1(btaudio_hangup_obj, btaudio_hangup_meth);

// dial(number=None): None redials the last number.
static mp_obj_t btaudio_dial_meth(size_t n_args, const mp_obj_t *args) {
    btaudio_check();
    const char *number = n_args > 1 && args[1] != mp_const_none ? mp_obj_str_get_str(args[1]) : NULL;
    return btaudio_done(btaudio_dial(number));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(btaudio_dial_obj, 1, 2, btaudio_dial_meth);

static mp_obj_t btaudio_voice_meth(mp_obj_t self_in, mp_obj_t on_in) {
    btaudio_check();
    return btaudio_done(btaudio_voice(mp_obj_is_true(on_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(btaudio_voice_obj, btaudio_voice_meth);

static mp_obj_t btaudio_dtmf_meth(mp_obj_t self_in, mp_obj_t digits_in) {
    btaudio_check();
    size_t n;
    const char *digits = mp_obj_str_get_data(digits_in, &n);
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = btaudio_dtmf(digits[i]);
        if (err != ESP_OK) {
            btaudio_raise(err);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(btaudio_dtmf_obj, btaudio_dtmf_meth);

// ---- the connection ----

static mp_obj_t btaudio_disconnect_meth(mp_obj_t self_in) {
    btaudio_check();
    return btaudio_done(btaudio_disconnect());
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

static mp_obj_t btaudio_cover_obj(void) {
    // The image can change between asking its size and copying it: retry.
    for (int tries = 0; tries < 3; tries++) {
        size_t size = btaudio_cover_size();
        if (size == 0) {
            return mp_const_none;
        }
        vstr_t v;
        vstr_init_len(&v, size);
        size_t n = btaudio_cover((uint8_t *)v.buf, size);
        if (n == size) {
            return mp_obj_new_bytes_from_vstr(&v);
        }
        vstr_clear(&v);
    }
    return mp_const_none;
}

// Properties: connected, streaming, playing, status, sample_rate, volume
// (settable), title, artist, album, genre, track_number, track_count,
// duration, position, cover, peer, peer_name.
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
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_TITLE);
                return;
            case MP_QSTR_artist:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_ARTIST);
                return;
            case MP_QSTR_album:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_ALBUM);
                return;
            case MP_QSTR_genre:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_GENRE);
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
                dest[0] = btaudio_cover_obj();
                return;
            case MP_QSTR_peer:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_PEER);
                return;
            case MP_QSTR_peer_name:
                dest[0] = btaudio_text_obj(BTAUDIO_TEXT_PEER_NAME);
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
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&btaudio_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_next), MP_ROM_PTR(&btaudio_next_obj) },
    { MP_ROM_QSTR(MP_QSTR_previous), MP_ROM_PTR(&btaudio_previous_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&btaudio_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&btaudio_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_setting), MP_ROM_PTR(&btaudio_setting_obj) },
    { MP_ROM_QSTR(MP_QSTR_refresh), MP_ROM_PTR(&btaudio_refresh_obj) },
    { MP_ROM_QSTR(MP_QSTR_link), MP_ROM_PTR(&btaudio_link_obj) },
    { MP_ROM_QSTR(MP_QSTR_phone), MP_ROM_PTR(&btaudio_phone_obj) },
    { MP_ROM_QSTR(MP_QSTR_answer), MP_ROM_PTR(&btaudio_answer_obj) },
    { MP_ROM_QSTR(MP_QSTR_hangup), MP_ROM_PTR(&btaudio_hangup_obj) },
    { MP_ROM_QSTR(MP_QSTR_dial), MP_ROM_PTR(&btaudio_dial_obj) },
    { MP_ROM_QSTR(MP_QSTR_voice), MP_ROM_PTR(&btaudio_voice_obj) },
    { MP_ROM_QSTR(MP_QSTR_dtmf), MP_ROM_PTR(&btaudio_dtmf_obj) },
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
    { MP_ROM_QSTR(MP_QSTR_POSITION), MP_ROM_INT(BTAUDIO_CHANGED_POSITION) },
    { MP_ROM_QSTR(MP_QSTR_PEER), MP_ROM_INT(BTAUDIO_CHANGED_PEER) },
    { MP_ROM_QSTR(MP_QSTR_COVER), MP_ROM_INT(BTAUDIO_CHANGED_COVER) },
    { MP_ROM_QSTR(MP_QSTR_PHONE), MP_ROM_INT(BTAUDIO_CHANGED_PHONE) },
    { MP_ROM_QSTR(MP_QSTR_CALL), MP_ROM_INT(BTAUDIO_CHANGED_CALL) },
    // AVRCP pass-through key codes for key(), beyond the named methods.
    { MP_ROM_QSTR(MP_QSTR_KEY_VOLUME_UP), MP_ROM_INT(0x41) },
    { MP_ROM_QSTR(MP_QSTR_KEY_VOLUME_DOWN), MP_ROM_INT(0x42) },
    { MP_ROM_QSTR(MP_QSTR_KEY_MUTE), MP_ROM_INT(0x43) },
    { MP_ROM_QSTR(MP_QSTR_KEY_REWIND), MP_ROM_INT(0x48) },
    { MP_ROM_QSTR(MP_QSTR_KEY_FAST_FORWARD), MP_ROM_INT(0x49) },
};
static MP_DEFINE_CONST_DICT(btaudio_module_globals, btaudio_module_table);

const mp_obj_module_t btaudio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&btaudio_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_btaudio, btaudio_module);
