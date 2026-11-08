// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.AudioSource: the board plays to headphones, a speaker or another
// board (A2DP source), and is the player a headset or car stereo controls
// (AVRCP target). It wraps the source half of btclassic.h and nothing else;
// mod_btclassic.c registers the type.

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/gc.h"
#include "py/runtime.h"

#include "btclassic.h"

extern const mp_obj_type_t btc_bluetooth_type;

static void src_raise(int err) {
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

static mp_obj_t src_done(int err) {
    if (err < 0) {
        src_raise(err);
    }
    return mp_const_none;
}

static void src_check(void) {
    if (!btc_source_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the source is stopped"));
    }
}

static void put(mp_obj_t d, qstr key, mp_obj_t value) {
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(key), value);
}

static mp_obj_t src_text(btc_source_text_t which) {
    char buf[128];
    size_t n = btc_source_text(which, buf, sizeof(buf));
    // A name cut to fit can end partway through a UTF-8 character.
    while (n > 0 && ((uint8_t)buf[n - 1] & 0xc0) == 0x80) {
        n--;
    }
    if (n > 0 && ((uint8_t)buf[n - 1] & 0xc0) == 0xc0) {
        n--;
    }
    return mp_obj_new_str(buf, n);
}

typedef struct {
    mp_obj_base_t base;
} src_obj_t;

// The program's format, which write() takes: the backend converts it to the
// stream's. Module state, like the source itself, so a soft reset keeps it.
static uint32_t src_rate = 44100;
static uint8_t src_channels = 2;

// The default buffer: 500 ms on a board with megabytes of heap (PSRAM),
// 250 ms otherwise, where 500 ms of 48 kHz stereo is 96 KB.
static mp_int_t src_default_ms(void) {
    gc_info_t info;
    gc_info(&info);
    return info.total > 1024 * 1024 ? 500 : 250;
}

// AudioSource(bt, *, rate=44100, channels=2, buffer_ms=None)
static mp_obj_t src_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_rate, ARG_channels, ARG_buffer_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 44100} },
        { MP_QSTR_channels, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2} },
        { MP_QSTR_buffer_ms, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    mp_int_t rate = args[ARG_rate].u_int;
    mp_int_t channels = args[ARG_channels].u_int;
    if (rate < 8000 || rate > 48000) {
        mp_raise_ValueError(MP_ERROR_TEXT("rate must be 8000 to 48000"));
    }
    if (channels != 1 && channels != 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("channels must be 1 or 2"));
    }
    if (!btc_radio_active()) {
        src_done(btc_radio_start());
    }
    if (!btc_source_started()) {
        if (btc_sink_started()) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("btclassic: an AudioSink is running; deinit() it first (one A2DP role at a time)"));
        }
        mp_obj_t ms_in = args[ARG_buffer_ms].u_obj;
        mp_int_t ms = ms_in == mp_const_none ? src_default_ms() : mp_obj_get_int(ms_in);
        if (ms < 50 || ms > 5000) {
            mp_raise_ValueError(MP_ERROR_TEXT("buffer_ms must be 50 to 5000"));
        }
        src_done(btc_source_start((size_t)(48 * ms)));
    }
    src_rate = (uint32_t)rate;
    src_channels = (uint8_t)channels;
    return MP_OBJ_FROM_PTR(mp_obj_malloc(src_obj_t, type));
}

// write(buf) -> bytes taken. Waits while the stream runs, so a program
// that writes as fast as it can is paced by it; never waits otherwise.
static mp_obj_t src_write(mp_obj_t self_in, mp_obj_t buf_in) {
    src_check();
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    size_t width = 2 * src_channels;
    size_t frames = buf.len / width;
    const int16_t *pcm = buf.buf;
    size_t done = 0;
    for (;;) {
        done += btc_source_write(pcm + done * src_channels, frames - done, src_rate, src_channels);
        if (done >= frames) {
            break;
        }
        btc_source_status_t st;
        btc_source_status(&st);
        if (!st.streaming) {
            break;
        }
        mp_event_wait_ms(5);
    }
    return MP_OBJ_NEW_SMALL_INT(done * width);
}
static MP_DEFINE_CONST_FUN_OBJ_2(src_write_obj, src_write);

static mp_obj_t src_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_source_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_changes_obj, src_changes);

static mp_obj_t src_stats(mp_obj_t self_in) {
    btc_ring_stats_t st;
    btc_source_stats(&st);
    mp_obj_t d = mp_obj_new_dict(7);
    put(d, MP_QSTR_buffered, mp_obj_new_int_from_uint(st.buffered));
    put(d, MP_QSTR_capacity, mp_obj_new_int_from_uint(st.capacity));
    put(d, MP_QSTR_received, mp_obj_new_int_from_uint(st.received));
    put(d, MP_QSTR_underruns, mp_obj_new_int_from_uint(st.underruns));
    put(d, MP_QSTR_overruns, mp_obj_new_int_from_uint(st.overruns));
    put(d, MP_QSTR_dropped, mp_obj_new_int_from_uint(st.dropped));
    put(d, MP_QSTR_repeated, mp_obj_new_int_from_uint(st.repeated));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_stats_obj, src_stats);

// key() -> (code, pressed) or None: the remote's keys, oldest first.
static mp_obj_t src_key(mp_obj_t self_in) {
    int k = btc_source_key();
    if (k < 0) {
        return mp_const_none;
    }
    mp_obj_t t[2] = { MP_OBJ_NEW_SMALL_INT(k & 0x7f), mp_obj_new_bool(k & 0x100) };
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_key_obj, src_key);

static const uint8_t *src_addr_arg(mp_obj_t obj) {
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

static mp_obj_t src_connect(size_t n_args, const mp_obj_t *args) {
    src_check();
    return src_done(btc_source_connect(src_addr_arg(n_args > 1 ? args[1] : mp_const_none)));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(src_connect_obj, 1, 2, src_connect);

static mp_obj_t src_disconnect(mp_obj_t self_in) {
    src_check();
    return src_done(btc_source_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_disconnect_obj, src_disconnect);

static mp_obj_t src_start(mp_obj_t self_in) {
    src_check();
    return src_done(btc_source_stream(true));
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_start_obj, src_start);

static mp_obj_t src_suspend(mp_obj_t self_in) {
    src_check();
    return src_done(btc_source_stream(false));
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_suspend_obj, src_suspend);

// A text argument: str, or None to leave the field as it is.
static const char *opt_str(mp_obj_t o) {
    return o == mp_const_none ? NULL : mp_obj_str_get_str(o);
}

static int opt_int(mp_obj_t o) {
    return o == mp_const_none ? BTC_UNKNOWN : (int)mp_obj_get_int(o);
}

// track(title=None, artist=None, album=None, genre=None, *, number=None,
//       count=None, duration=None)
static mp_obj_t src_track(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_title, ARG_artist, ARG_album, ARG_genre, ARG_number, ARG_count, ARG_duration };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_title, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_artist, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_album, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_genre, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_number, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_count, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_duration, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    src_check();
    mp_obj_t dur = args[ARG_duration].u_obj;
    return src_done(btc_source_set_track(opt_str(args[ARG_title].u_obj), opt_str(args[ARG_artist].u_obj),
        opt_str(args[ARG_album].u_obj), opt_str(args[ARG_genre].u_obj),
        opt_int(args[ARG_number].u_obj), opt_int(args[ARG_count].u_obj),
        dur == mp_const_none ? BTC_UNKNOWN : (int64_t)mp_obj_get_int(dur)));
}
static MP_DEFINE_CONST_FUN_OBJ_KW(src_track_obj, 1, src_track);

static const qstr status_names[] = {
    MP_QSTR_stopped, MP_QSTR_playing, MP_QSTR_paused,
};

static mp_obj_t src_link(mp_obj_t self_in) {
    btc_source_status_t st;
    btc_source_status(&st);
    mp_obj_t d = mp_obj_new_dict(14);
    put(d, MP_QSTR_address, st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none);
    put(d, MP_QSTR_name, src_text(BTC_SOURCE_PEER_NAME));
    put(d, MP_QSTR_codec, st.sample_rate ? MP_OBJ_NEW_QSTR(MP_QSTR_SBC) : mp_const_none);
    put(d, MP_QSTR_sample_rate, mp_obj_new_int_from_uint(st.sample_rate));
    put(d, MP_QSTR_channels, MP_OBJ_NEW_SMALL_INT(st.channels));
    uint8_t m = st.sbc_channel_mode;
    put(d, MP_QSTR_channel_mode, m & 0x8 ? MP_OBJ_NEW_QSTR(MP_QSTR_mono) : m & 0x4 ? MP_OBJ_NEW_QSTR(MP_QSTR_dual)
        : m & 0x2 ? MP_OBJ_NEW_QSTR(MP_QSTR_stereo) : m & 0x1 ? mp_obj_new_str("joint stereo", 12) : mp_const_none);
    put(d, MP_QSTR_bitpool_min, MP_OBJ_NEW_SMALL_INT(st.sbc_min_bitpool));
    put(d, MP_QSTR_bitpool_max, MP_OBJ_NEW_SMALL_INT(st.sbc_max_bitpool));
    put(d, MP_QSTR_delay_ms, MP_OBJ_NEW_SMALL_INT(st.delay_ms));
    put(d, MP_QSTR_avrcp, mp_obj_new_bool(st.avrcp));
    put(d, MP_QSTR_absolute_volume, mp_obj_new_bool(st.absolute_volume));
    put(d, MP_QSTR_metadata, mp_obj_new_bool(st.metadata));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_link_obj, src_link);

static mp_obj_t src_deinit(mp_obj_t self_in) {
    btc_source_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(src_deinit_obj, src_deinit);

static mp_obj_t src_exit(size_t n_args, const mp_obj_t *args) {
    btc_source_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(src_exit_obj, 4, 4, src_exit);

static void src_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_source_status_t st;
        btc_source_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_streaming:
                dest[0] = mp_obj_new_bool(st.streaming);
                return;
            case MP_QSTR_sample_rate:
                dest[0] = mp_obj_new_int_from_uint(st.sample_rate);
                return;
            case MP_QSTR_rate:
                dest[0] = mp_obj_new_int_from_uint(src_rate);
                return;
            case MP_QSTR_channels:
                dest[0] = MP_OBJ_NEW_SMALL_INT(src_channels);
                return;
            case MP_QSTR_volume:
                dest[0] = MP_OBJ_NEW_SMALL_INT(st.volume);
                return;
            case MP_QSTR_peer:
                dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
                return;
            case MP_QSTR_peer_name:
                dest[0] = src_text(BTC_SOURCE_PEER_NAME);
                return;
            case MP_QSTR_status:
                dest[0] = st.play_status >= 0 && st.play_status < (int)MP_ARRAY_SIZE(status_names)
                    ? MP_OBJ_NEW_QSTR(status_names[st.play_status]) : mp_const_none;
                return;
            case MP_QSTR_position:
                dest[0] = st.position_ms < 0 ? mp_const_none : mp_obj_new_int_from_ll(st.position_ms);
                return;
            case MP_QSTR_title:
                dest[0] = src_text(BTC_SOURCE_TITLE);
                return;
            case MP_QSTR_artist:
                dest[0] = src_text(BTC_SOURCE_ARTIST);
                return;
            default:
                dest[1] = MP_OBJ_SENTINEL;  // methods, from the locals dict
                return;
        }
    } else if (dest[1] != MP_OBJ_NULL) {
        mp_obj_t v = dest[1];
        switch (attr) {
            case MP_QSTR_volume: {
                mp_int_t vol = mp_obj_get_int(v);
                if (vol < 0 || vol > 127) {
                    mp_raise_ValueError(MP_ERROR_TEXT("volume must be 0 to 127"));
                }
                btc_source_set_volume((uint8_t)vol);
                break;
            }
            case MP_QSTR_rate: {
                mp_int_t r = mp_obj_get_int(v);
                if (r < 8000 || r > 48000) {
                    mp_raise_ValueError(MP_ERROR_TEXT("rate must be 8000 to 48000"));
                }
                src_rate = (uint32_t)r;
                break;
            }
            case MP_QSTR_channels: {
                mp_int_t c = mp_obj_get_int(v);
                if (c != 1 && c != 2) {
                    mp_raise_ValueError(MP_ERROR_TEXT("channels must be 1 or 2"));
                }
                src_channels = (uint8_t)c;
                break;
            }
            case MP_QSTR_status: {
                src_check();
                qstr q = mp_obj_str_get_qstr(v);
                int status = -1;
                for (size_t i = 0; i < MP_ARRAY_SIZE(status_names); i++) {
                    if (status_names[i] == q) {
                        status = (int)i;
                    }
                }
                if (status < 0) {
                    mp_raise_ValueError(MP_ERROR_TEXT("status is stopped, playing or paused"));
                }
                src_done(btc_source_set_play_status(status, BTC_UNKNOWN));
                break;
            }
            case MP_QSTR_position: {
                src_check();
                mp_int_t ms = mp_obj_get_int(v);
                if (ms < 0) {
                    mp_raise_ValueError(NULL);
                }
                // Keep the status; move the position.
                src_done(btc_source_set_play_status(BTC_UNKNOWN, ms));
                break;
            }
            default:
                return;     // read only: AttributeError
        }
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t src_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&src_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&src_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&src_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_key), MP_ROM_PTR(&src_key_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&src_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&src_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&src_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_suspend), MP_ROM_PTR(&src_suspend_obj) },
    { MP_ROM_QSTR(MP_QSTR_track), MP_ROM_PTR(&src_track_obj) },
    { MP_ROM_QSTR(MP_QSTR_link), MP_ROM_PTR(&src_link_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&src_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&src_exit_obj) },
};
static MP_DEFINE_CONST_DICT(src_locals, src_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_source_type,
    MP_QSTR_AudioSource,
    MP_TYPE_FLAG_NONE,
    make_new, src_make_new,
    attr, src_attr,
    locals_dict, &src_locals
    );
