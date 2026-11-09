// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.L2CAP: a raw L2CAP channel, as a stream like machine.UART. It
// wraps the L2CAP part of btclassic.h and nothing else; mod_btclassic.c
// registers the type.

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
} l2_obj_t;

// How long read() waits for its first byte, in ms. Module state, like the
// port itself, so a soft reset keeps it.
static mp_int_t l2_timeout = 0;

static mp_obj_t l2_done(int err) {
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

// A PSM's low byte is odd and its high byte even; 0x1001 and up are free for
// anyone's protocol.
static bool l2_psm_ok(mp_int_t psm) {
    return psm >= 0x1001 && psm <= 0xffff && (psm & 0x0001) && !(psm & 0x0100);
}

static void l2_check(void) {
    if (!btc_l2cap_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the L2CAP channel is closed"));
    }
}

// L2CAP(bt, psm=None, *, rxbuf=4096, timeout=0)
static mp_obj_t l2_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_psm, ARG_rxbuf, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_psm, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rxbuf, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 4096} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    mp_int_t psm = args[ARG_psm].u_obj == mp_const_none ? 0 : mp_obj_get_int(args[ARG_psm].u_obj);
    if (psm != 0 && !l2_psm_ok(psm)) {
        mp_raise_ValueError(MP_ERROR_TEXT("a PSM is odd, 0x1001 to 0xffff, or None"));
    }
    mp_int_t rxbuf = args[ARG_rxbuf].u_int;
    if (rxbuf < 256 || rxbuf > 65536) {
        mp_raise_ValueError(MP_ERROR_TEXT("rxbuf must be 256 to 65536"));
    }
    if (args[ARG_timeout].u_int < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("timeout must be 0 or more"));
    }
    if (btc_l2cap_started()) {
        btc_l2cap_status_t st;
        btc_l2cap_status(&st);
        if (st.psm != psm) {
            btc_l2cap_stop();
        }
    }
    if (!btc_radio_active()) {
        l2_done(btc_radio_start());
    }
    if (!btc_l2cap_started()) {
        l2_done(btc_l2cap_start((uint16_t)psm, (size_t)rxbuf));
        // The server comes up a moment later; wait for it so listening is set.
        mp_uint_t start = mp_hal_ticks_ms();
        btc_l2cap_status_t st;
        do {
            mp_event_wait_ms(10);
            btc_l2cap_status(&st);
        } while (psm && !st.listening && !st.connected && mp_hal_ticks_ms() - start < 1000);
    }
    l2_timeout = args[ARG_timeout].u_int;
    return MP_OBJ_FROM_PTR(mp_obj_malloc(l2_obj_t, type));
}

// ---- the stream ----

static mp_uint_t l2_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    if (!btc_l2cap_started()) {
        *errcode = MP_EIO;
        return MP_STREAM_ERROR;
    }
    if (size == 0) {
        return 0;
    }
    // Wait up to the timeout for the first byte, then take what's there.
    mp_uint_t start = mp_hal_ticks_ms();
    while (btc_l2cap_any() == 0) {
        if (!btc_l2cap_connected() || (mp_int_t)(mp_hal_ticks_ms() - start) >= l2_timeout) {
            *errcode = MP_EAGAIN;
            return MP_STREAM_ERROR;
        }
        mp_event_wait_ms(5);
    }
    return btc_l2cap_read(buf, size);
}

static mp_uint_t l2_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    if (!btc_l2cap_connected()) {
        *errcode = MP_ENOTCONN;
        return MP_STREAM_ERROR;
    }
    // Wait while the link is up until all of it is queued to send.
    const uint8_t *src = buf;
    mp_uint_t done = 0;
    for (;;) {
        done += btc_l2cap_write(src + done, size - done);
        if (done >= size || !btc_l2cap_connected()) {
            break;
        }
        mp_event_wait_ms(2);
    }
    return done;
}

static mp_uint_t l2_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    if (request == MP_STREAM_POLL) {
        mp_uint_t ret = 0;
        if ((arg & MP_STREAM_POLL_RD) && btc_l2cap_any() > 0) {
            ret |= MP_STREAM_POLL_RD;
        }
        if ((arg & MP_STREAM_POLL_WR) && btc_l2cap_write_room() > 0) {
            ret |= MP_STREAM_POLL_WR;
        }
        return ret;
    }
    if (request == MP_STREAM_FLUSH) {
        return 0;
    }
    if (request == MP_STREAM_CLOSE) {
        btc_l2cap_stop();
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_stream_p_t l2_stream_p = {
    .read = l2_read,
    .write = l2_write,
    .ioctl = l2_ioctl,
    .is_text = false,
};

// ---- methods ----

static mp_obj_t l2_any(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_l2cap_any());
}
static MP_DEFINE_CONST_FUN_OBJ_1(l2_any_obj, l2_any);

static mp_obj_t l2_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_l2cap_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(l2_changes_obj, l2_changes);

// connect(addr, psm, timeout_ms=10000): call another device's PSM and wait
// until the channel is open.
static mp_obj_t l2_connect(size_t n_args, const mp_obj_t *args) {
    l2_check();
    mp_buffer_info_t addr;
    mp_get_buffer_raise(args[1], &addr, MP_BUFFER_READ);
    if (addr.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
    }
    mp_int_t psm = mp_obj_get_int(args[2]);
    if (!l2_psm_ok(psm) && psm != 0x0001 && !(psm >= 0x0003 && psm < 0x1000 && (psm & 1))) {
        mp_raise_ValueError(MP_ERROR_TEXT("a PSM is odd, 0x1001 to 0xffff, or None"));
    }
    mp_int_t timeout = n_args > 3 ? mp_obj_get_int(args[3]) : 10000;
    if (btc_l2cap_connected()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: already connected"));
    }
    l2_done(btc_l2cap_connect(addr.buf, (uint16_t)psm));
    mp_uint_t start = mp_hal_ticks_ms();
    while (btc_l2cap_calling() && !btc_l2cap_connected()) {
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(10);
    }
    if (!btc_l2cap_connected()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: no answer from the device"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(l2_connect_obj, 3, 4, l2_connect);

static mp_obj_t l2_disconnect(mp_obj_t self_in) {
    l2_check();
    return l2_done(btc_l2cap_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(l2_disconnect_obj, l2_disconnect);

static mp_obj_t l2_stats(mp_obj_t self_in) {
    btc_l2cap_status_t st;
    btc_l2cap_status(&st);
    mp_obj_t d = mp_obj_new_dict(3);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_bytes), mp_obj_new_int_from_uint(st.rx_bytes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_bytes), mp_obj_new_int_from_uint(st.tx_bytes));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_dropped), mp_obj_new_int_from_uint(st.rx_dropped));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(l2_stats_obj, l2_stats);

static mp_obj_t l2_deinit(mp_obj_t self_in) {
    btc_l2cap_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(l2_deinit_obj, l2_deinit);

static mp_obj_t l2_exit(size_t n_args, const mp_obj_t *args) {
    return l2_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(l2_exit_obj, 4, 4, l2_exit);

static void l2_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL) {
        btc_l2cap_status_t st;
        btc_l2cap_status(&st);
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
            case MP_QSTR_psm:
                dest[0] = st.psm ? MP_OBJ_NEW_SMALL_INT(st.psm) : mp_const_none;
                return;
            case MP_QSTR_mtu:
                dest[0] = st.connected && st.mtu ? MP_OBJ_NEW_SMALL_INT(st.mtu) : mp_const_none;
                return;
            case MP_QSTR_timeout:
                dest[0] = MP_OBJ_NEW_SMALL_INT(l2_timeout);
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
        l2_timeout = t;
        dest[0] = MP_OBJ_NULL;
    }
}

static const mp_rom_map_elem_t l2_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_any), MP_ROM_PTR(&l2_any_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&l2_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&l2_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&l2_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&l2_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&l2_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&l2_exit_obj) },
};
static MP_DEFINE_CONST_DICT(l2_locals, l2_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_l2cap_type,
    MP_QSTR_L2CAP,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, l2_make_new,
    attr, l2_attr,
    protocol, &l2_stream_p,
    locals_dict, &l2_locals
    );
