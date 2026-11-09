// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.ObjectPush: sending a file or contact card to a phone or
// computer, pulling its owner's card, and taking what it sends (OPP). It
// wraps the object push half of btclassic.h; mod_btclassic.c registers the
// type. Only BTstack (the Pico W) has it: on the ESP32 the class is there,
// and making one raises.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "btclassic.h"

typedef struct {
    mp_obj_base_t base;
} opp_obj_t;

#if BTC_HAS_PUSH

extern const mp_obj_type_t btc_bluetooth_type;

static mp_obj_t opp_done(int err) {
    switch (err) {
        case BTC_OK:
            return mp_const_none;
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_ARG:
            mp_raise_ValueError(MP_ERROR_TEXT("btclassic: a name is 1 to 63 bytes, a type up to 47"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: busy, no device to send to, or the device closed the session"));
        case BTC_ERR_FAIL:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: couldn't reach the device's Object Push"));
        case 0xc1:
        case 0xc3:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the device refused it"));
        case 0xc0:      // Android's answer to a pull
        case 0xc4:
        case 0xd1:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the device has nothing to give"));
        case 0xc6:
        case 0xcf:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the device doesn't take that type"));
        case 0xcd:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: too big for the device"));
        case 0xd3:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the device is busy"));
        default:
            mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the device said 0x%02x"), err);
    }
}

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

// Wait for the send or pull; raise on timeout or a refusal.
static void opp_wait(mp_int_t timeout_ms) {
    mp_uint_t start = mp_hal_ticks_ms();
    btc_opp_status_t st;
    for (;;) {
        btc_opp_status(&st);
        if (!st.busy) {
            break;
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout_ms) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(5);
    }
    opp_done(st.result);
}

static mp_obj_t held_bytes(void) {
    btc_opp_status_t st;
    btc_opp_status(&st);
    vstr_t v;
    vstr_init_len(&v, st.length);
    v.len = btc_opp_data((uint8_t *)v.buf, st.length);
    return mp_obj_new_bytes_from_vstr(&v);
}

// ObjectPush(bt, *, max_bytes=65536)
static mp_obj_t opp_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_max_bytes };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_max_bytes, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 65536} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    if (!btc_radio_active()) {
        opp_done(btc_radio_start());
    }
    if (!btc_opp_started()) {
        mp_int_t max = args[ARG_max_bytes].u_int;
        if (max < 1024) {
            mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be 1024 or more"));
        }
        opp_done(btc_opp_start((size_t)max));
    }
    return MP_OBJ_FROM_PTR(mp_obj_malloc(opp_obj_t, type));
}

// send(name, data, *, addr=None, type=None, timeout=60000)
static mp_obj_t opp_send(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_name, ARG_data, ARG_addr, ARG_type, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_data, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_addr, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_type, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 60000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    const char *name = mp_obj_str_get_str(args[ARG_name].u_obj);
    const char *type = args[ARG_type].u_obj == mp_const_none ? NULL : mp_obj_str_get_str(args[ARG_type].u_obj);
    mp_buffer_info_t data;
    mp_get_buffer_raise(args[ARG_data].u_obj, &data, MP_BUFFER_READ);
    btc_opp_changes();
    opp_done(btc_opp_send(addr_arg(args[ARG_addr].u_obj), name, type, data.buf, data.len));
    opp_wait(args[ARG_timeout].u_int);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(opp_send_obj, 3, opp_send);

// pull(*, addr=None, timeout=30000): the device owner's card, as bytes.
static mp_obj_t opp_pull(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_addr, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_addr, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    btc_opp_status_t st;
    btc_opp_status(&st);
    if (st.held) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: take the received object first"));
    }
    btc_opp_changes();
    opp_done(btc_opp_pull(addr_arg(args[ARG_addr].u_obj)));
    opp_wait(args[ARG_timeout].u_int);
    mp_obj_t card = held_bytes();
    btc_opp_take();
    return card;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(opp_pull_obj, 1, opp_pull);

// listen(on=True)
static mp_obj_t opp_listen(size_t n_args, const mp_obj_t *args) {
    bool on = n_args < 2 || mp_obj_is_true(args[1]);
    return opp_done(btc_opp_listen(on));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(opp_listen_obj, 1, 2, opp_listen);

// received(): (name, type, data) of what a device sent, or None; taking it
// lets the next one in.
static mp_obj_t opp_received(mp_obj_t self_in) {
    btc_opp_status_t st;
    btc_opp_status(&st);
    if (!st.held) {
        return mp_const_none;
    }
    const char *type = btc_opp_type();
    mp_obj_t t[3] = {
        mp_obj_new_str(btc_opp_name(), strlen(btc_opp_name())),
        type[0] ? mp_obj_new_str(type, strlen(type)) : mp_const_none,
        held_bytes(),
    };
    btc_opp_take();
    return mp_obj_new_tuple(3, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(opp_received_obj, opp_received);

static mp_obj_t opp_changes(mp_obj_t self_in) {
    return mp_obj_new_int_from_uint(btc_opp_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(opp_changes_obj, opp_changes);

static mp_obj_t opp_deinit(mp_obj_t self_in) {
    btc_opp_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(opp_deinit_obj, opp_deinit);

static mp_obj_t opp_exit(size_t n_args, const mp_obj_t *args) {
    btc_opp_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(opp_exit_obj, 4, 4, opp_exit);

static void opp_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    btc_opp_status_t st;
    btc_opp_status(&st);
    switch (attr) {
        case MP_QSTR_busy:
            dest[0] = mp_obj_new_bool(st.busy);
            return;
        case MP_QSTR_listening:
            dest[0] = mp_obj_new_bool(st.listening);
            return;
        case MP_QSTR_receiving:
            dest[0] = mp_obj_new_bool(st.receiving);
            return;
        case MP_QSTR_truncated:
            dest[0] = mp_obj_new_bool(st.truncated);
            return;
        case MP_QSTR_peer:
            dest[0] = mp_obj_new_bytes(st.peer, 6);
            return;
        case MP_QSTR_sent:
            dest[0] = mp_obj_new_int_from_uint(st.sent);
            return;
        case MP_QSTR_refused:
            dest[0] = mp_obj_new_int_from_uint(st.refused);
            return;
        default:
            dest[1] = MP_OBJ_SENTINEL;
            return;
    }
}

static const mp_rom_map_elem_t opp_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&opp_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull), MP_ROM_PTR(&opp_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&opp_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_received), MP_ROM_PTR(&opp_received_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&opp_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&opp_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&opp_exit_obj) },
};
static MP_DEFINE_CONST_DICT(opp_locals, opp_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_push_type,
    MP_QSTR_ObjectPush,
    MP_TYPE_FLAG_NONE,
    make_new, opp_make_new,
    attr, opp_attr,
    locals_dict, &opp_locals
    );

#else

// No OPP in this backend: the class is there so code can check for it, and
// making one says why it can't work.
static mp_obj_t opp_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: ObjectPush needs the Pico W; this board's Bluetooth stack has no OPP"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    btc_push_type,
    MP_QSTR_ObjectPush,
    MP_TYPE_FLAG_NONE,
    make_new, opp_make_new
    );

#endif // BTC_HAS_PUSH
