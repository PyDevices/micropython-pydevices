// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.Tether: the internet through a phone's Bluetooth tethering (PAN),
// as a network interface like network.WLAN. It wraps the tether half of
// btclassic.h; mod_btclassic.c registers the type. Only BTstack (the Pico W)
// has a PAN user: on the ESP32 the class is there, and making one raises.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "btclassic.h"

typedef struct {
    mp_obj_base_t base;
} tether_obj_t;

#if BTC_HAS_TETHER

#include "extmod/modnetwork.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"

extern const mp_obj_type_t btc_bluetooth_type;

static mp_obj_t te_done(int err) {
    switch (err) {
        case BTC_OK:
            return mp_const_none;
        case BTC_ERR_NOMEM:
            mp_raise_OSError(MP_ENOMEM);
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_STATE:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: no phone to tether to; pair one or pass its address"));
        default:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the phone refused; turn on its Bluetooth tethering"));
    }
}

static struct netif *te_netif(void) {
    struct netif *n = btc_tether_netif();
    if (!n || !btc_tether_started()) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the tether is stopped"));
    }
    return n;
}

static bool te_has_address(struct netif *n) {
    return netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));
}

// Tether(bt)
static mp_obj_t te_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    if (!mp_obj_is_type(all_args[0], &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    if (!btc_radio_active()) {
        te_done(btc_radio_start());
    }
    te_done(btc_tether_start());
    return MP_OBJ_FROM_PTR(mp_obj_malloc(tether_obj_t, type));
}

// connect(addr=None, *, timeout=30000): up, with an address from the phone.
static mp_obj_t te_connect(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_addr, ARG_timeout };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_addr, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    struct netif *n = te_netif();
    const uint8_t *addr = NULL;
    if (args[ARG_addr].u_obj != mp_const_none) {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(args[ARG_addr].u_obj, &buf, MP_BUFFER_READ);
        if (buf.len != 6) {
            mp_raise_ValueError(MP_ERROR_TEXT("an address is 6 bytes"));
        }
        addr = buf.buf;
    }
    btc_tether_changes();
    te_done(btc_tether_connect(addr));
    mp_int_t timeout = args[ARG_timeout].u_int;
    if (timeout <= 0) {
        return mp_const_none;
    }
    mp_uint_t start = mp_hal_ticks_ms();
    btc_tether_status_t st;
    for (;;) {
        btc_tether_status(&st);
        if (st.connected && te_has_address(n)) {
            return mp_const_none;
        }
        if (!st.connected && !st.connecting) {
            te_done(st.result ? st.result : BTC_ERR_FAIL);
        }
        if ((mp_int_t)(mp_hal_ticks_ms() - start) >= timeout) {
            mp_raise_OSError(MP_ETIMEDOUT);
        }
        mp_event_wait_ms(10);
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(te_connect_obj, 1, te_connect);

static mp_obj_t te_disconnect(mp_obj_t self_in) {
    te_netif();
    return te_done(btc_tether_disconnect());
}
static MP_DEFINE_CONST_FUN_OBJ_1(te_disconnect_obj, te_disconnect);

static mp_obj_t te_isconnected(mp_obj_t self_in) {
    struct netif *n = btc_tether_netif();
    return mp_obj_new_bool(n && btc_tether_started() && te_has_address(n));
}
static MP_DEFINE_CONST_FUN_OBJ_1(te_isconnected_obj, te_isconnected);

static mp_obj_t te_ifconfig(size_t n_args, const mp_obj_t *args) {
    return mod_network_nic_ifconfig(te_netif(), n_args - 1, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(te_ifconfig_obj, 1, 2, te_ifconfig);

static mp_obj_t te_ipconfig(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    return mod_network_nic_ipconfig(te_netif(), n_args - 1, args + 1, kwargs);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(te_ipconfig_obj, 1, te_ipconfig);

static mp_obj_t te_stats(mp_obj_t self_in) {
    btc_tether_status_t st;
    btc_tether_status(&st);
    mp_obj_t d = mp_obj_new_dict(3);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_frames), mp_obj_new_int_from_uint(st.rx_frames));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_frames), mp_obj_new_int_from_uint(st.tx_frames));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_dropped), mp_obj_new_int_from_uint(st.tx_dropped));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(te_stats_obj, te_stats);

static mp_obj_t te_changes(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(btc_tether_changes());
}
static MP_DEFINE_CONST_FUN_OBJ_1(te_changes_obj, te_changes);

static mp_obj_t te_deinit(mp_obj_t self_in) {
    btc_tether_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(te_deinit_obj, te_deinit);

static mp_obj_t te_exit(size_t n_args, const mp_obj_t *args) {
    btc_tether_stop();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(te_exit_obj, 4, 4, te_exit);

static void te_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    btc_tether_status_t st;
    btc_tether_status(&st);
    switch (attr) {
        case MP_QSTR_connected:
            dest[0] = mp_obj_new_bool(st.connected);
            return;
        case MP_QSTR_peer:
            dest[0] = st.connected ? mp_obj_new_bytes(st.peer, 6) : mp_const_none;
            return;
        default:
            dest[1] = MP_OBJ_SENTINEL;
            return;
    }
}

static const mp_rom_map_elem_t te_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&te_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_disconnect), MP_ROM_PTR(&te_disconnect_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&te_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&te_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&te_ipconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&te_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_changes), MP_ROM_PTR(&te_changes_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&te_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&te_exit_obj) },
};
static MP_DEFINE_CONST_DICT(te_locals, te_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_tether_type,
    MP_QSTR_Tether,
    MP_TYPE_FLAG_NONE,
    make_new, te_make_new,
    attr, te_attr,
    locals_dict, &te_locals
    );

#else

// No PAN user in this backend: the class is there so code can check for it,
// and making one says why it can't work.
static mp_obj_t te_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: Tether needs the Pico W; this board's Bluetooth stack has no PAN"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    btc_tether_type,
    MP_QSTR_Tether,
    MP_TYPE_FLAG_NONE,
    make_new, te_make_new
    );

#endif // BTC_HAS_TETHER
