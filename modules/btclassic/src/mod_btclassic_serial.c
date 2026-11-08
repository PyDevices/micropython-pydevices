// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.Serial: a serial port over Bluetooth (SPP), as a stream like
// machine.UART. It wraps the serial half of btclassic.h and nothing else;
// mod_btclassic.c registers the type.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "btclassic.h"

extern const mp_obj_type_t btc_bluetooth_type;

typedef struct {
    mp_obj_base_t base;
} ser_obj_t;

// How long read() waits for its first byte, in ms. Module state, like the
// port itself, so a soft reset keeps it.
static mp_int_t ser_timeout = 0;

static mp_obj_t ser_done(int err) {
    switch (err) {
        case BTC_OK:
            return mp_const_none;
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: busy, or not connected"));
        case BTC_ERR_ARG:
            mp_raise_ValueError(NULL);
        default:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the Bluetooth stack refused"));
    }
}

static void ser_check(void) {
    if (!btc_spp_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the serial port is stopped"));
    }
}

// Serial(bt, *, name="Serial Port", rxbuf=4096, timeout=0)
static mp_obj_t ser_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_name, ARG_rxbuf, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_name, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rxbuf, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 4096} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    mp_int_t rxbuf = args[ARG_rxbuf].u_int;
    if (rxbuf < 256 || rxbuf > 65536) {
        mp_raise_ValueError(MP_ERROR_TEXT("rxbuf must be 256 to 65536"));
    }
    if (args[ARG_timeout].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0 or more"));
    }
    if (!btc_radio_active()) {
        ser_done(btc_radio_start());
    }
    if (!btc_spp_started()) {
        mp_obj_t name = args[ARG_name].u_obj;
        ser_done(btc_spp_start(name == mp_const_none ? NULL : mp_obj_str_get_str(name), (size_t)rxbuf));
    }
    ser_timeout = args[ARG_timeout].u_int;
    return MP_OBJ_FROM_PTR(mp_obj_malloc(ser_obj_t, type));
}

// ---- the stream ----

static mp_uint_t ser_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    if (!btc_spp_started()) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    if (size == 0) {
        return 0;
    }
    // Wait up to the timeout for the first byte, then take what's there.
    mp_uint_t start = mp_hal_ticks_ms();
    while (btc_spp_any() == 0) {
        if (!btc_spp_connected() || (mp_int_t)(mp_hal_ticks_ms() - start) >= ser_timeout) {
            *errcode = MP_EAGAIN;
            return MP_STREAM_ERROR;
        }
        mp_event_wait_ms(5);
    }
    return btc_spp_read(buf, size);
}

static mp_uint_t ser_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    if (!btc_spp_connected()) {
        *errcode = MP_ENOTCONN;
        return MP_STREAM_ERROR;
    }
    // Wait while the link is up until all of it is queued to send.
    const uint8_t *src = buf;
    mp_uint_t done = 0;
    for (;;) {
        done += btc_spp_write(src + done, size - done);
        if (done >= size || !btc_spp_connected()) {
            break;
        }
        mp_event_wait_ms(2);
    }
    return done;
}

static mp_uint_t ser_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    if (request == MP_STREAM_POLL) {
        mp_uint_t ret = 0;
        if ((arg & MP_STREAM_POLL_RD) && btc_spp_any() > 0) {
            ret |= MP_STREAM_POLL_RD;
        }
        if ((arg & MP_STREAM_POLL_WR) && btc_spp_write_room() > 0) {
            ret |= MP_STREAM_POLL_WR;
        }
        return ret;
    }
    if (request == MP_STREAM_FLUSH) {
        return 0;
    }
    if (request == MP_STREAM_CLOSE) {
        btc_spp_stop();
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_stream_p_t ser_stream_p = {
    .read = ser_read,
    .write = ser_write,
    .ioctl = ser_ioctl,
    .is_text = false,
};

// ---- methods ----

static mp_obj_t ser_any(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_spp_any());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ser_any_obj, ser_any);

static mp_obj_t ser_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_spp_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ser_changes_obj, ser_changes);

// connect(addr, channel=0): 0 finds the device's serial port.
static mp_obj_t ser_connect(size_t n_args, const mp_obj_t *args) {
    ser_check();
    mp_buffer_info_t addr;
    mp_get_buffer_raise(args[1], &addr, MP_BUFFER_READ);
    if (addr.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
    }
    mp_int_t channel = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
    if (channel < 0 || channel > 30) {
        mp_raise_ValueError(MP_ERROR_TEXT("channel must be 1 to 30, or 0 to look it up"));
    }
    return ser_done(btc_spp_connect(addr.buf, (int)channel));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ser_connect_obj, 2, 3, ser_connect);

static mp_obj_t ser_disconnect(mp_obj_t self_in) {
    ser_check();
    return ser_done(btc_spp_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(ser_disconnect_obj, ser_disconnect);

static mp_obj_t ser_stats(mp_obj_t self_in) {
    btc_spp_status_t st;
    btc_spp_status(&st);
    mp_obj_t d = mp_obj_new_dict(3);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_bytes), mp_obj_new_int_from_uint(st.rx_bytes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_bytes), mp_obj_new_int_from_uint(st.tx_bytes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_dropped), mp_obj_new_int_from_uint(st.rx_dropped));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ser_stats_obj, ser_stats);

static mp_obj_t ser_deinit(mp_obj_t self_in) {
    btc_spp_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ser_deinit_obj, ser_deinit);

static mp_obj_t ser_exit(size_t n_args, const mp_obj_t *args) {
    return ser_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ser_exit_obj, 4, 4, ser_exit);

static void ser_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_spp_status_t st;
        btc_spp_status(&st);
        switch (attr) {
            case MP_QSTR_connected:
                dest[0] = mp_obj_new_bool(st.connected);
                return;
            case MP_QSTR_listening:
                dest[0] = mp_obj_new_bool(st.listening);
                return;
            case MP_QSTR_peer:
                dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
                return;
            case MP_QSTR_channel:
                dest[0] = st.channel ? MP_OBJ_NEW_SMALL_INT(st.channel) : mp_const_none;
                return;
            case MP_QSTR_timeout:
                dest[0] = MP_OBJ_NEW_SMALL_INT(ser_timeout);
                return;
            default:
                dest[1] = MP_OBJ_SENTINEL;  // methods, from the locals dict
                return;
        }
    } else if (dest[1] != MP_OBJ_NULL && attr == MP_QSTR_timeout) {
        mp_int_t t = mp_obj_get_int(dest[1]);
        if (t < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0 or more"));
        }
        ser_timeout = t;
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t ser_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&ser_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&ser_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&ser_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&ser_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&ser_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&ser_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&ser_exit_obj) },
};
static MP_DEFINE_CONST_DICT(ser_locals, ser_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_serial_type,
    MP_QSTR_Serial,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, ser_make_new,
    attr, ser_attr,
    protocol, &ser_stream_p,
    locals_dict, &ser_locals
    );
