// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic.Service: an SDP record of the program's own, so a device that
// searches finds a Serial port or L2CAP channel by a UUID and a name. It
// wraps the service half of btclassic.h; mod_btclassic.c registers the type.
// Where a build can't make records, the class is there and making one raises.

#include <string.h>

#include "py/mperrno.h"
#include "py/obj.h"
#include "py/runtime.h"

#include "btclassic.h"

extern const mp_obj_type_t btc_bluetooth_type;

typedef struct {
    mp_obj_base_t base;
    int slot;       // -1 once closed
} svc_obj_t;

// A UUID is an int (16 or 32 bits) or 16 bytes, most significant first: the
// same forms Bluetooth.services() returns.
static void svc_uuid(mp_obj_t obj, btc_uuid_t *uuid) {
    memset(uuid, 0, sizeof(*uuid));
    if (mp_obj_is_int(obj)) {
        mp_uint_t v = mp_obj_get_int_truncated(obj);
        if (v == 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("a UUID is 16 or 32 bits, or 16 bytes"));
        }
        uuid->len = v > 0xffff ? 4 : 2;
        for (int i = 0; i < uuid->len; i++) {
            uuid->uuid[uuid->len - 1 - i] = (uint8_t)(v >> (8 * i));
        }
        return;
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(obj, &b, MP_BUFFER_READ);
    if (b.len != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("a UUID is 16 or 32 bits, or 16 bytes"));
    }
    uuid->len = 16;
    memcpy(uuid->uuid, b.buf, 16);
}

// Service(bt, uuid, name, *, channel=None, psm=None)
static mp_obj_t svc_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bt, ARG_uuid, ARG_name, ARG_channel, ARG_psm };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bt, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_uuid, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_name, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_channel, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_psm, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (!mp_obj_is_type(args[ARG_bt].u_obj, &btc_bluetooth_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected btclassic.Bluetooth()"));
    }
    btc_uuid_t uuid;
    svc_uuid(args[ARG_uuid].u_obj, &uuid);
    size_t name_len;
    const char *name = mp_obj_str_get_data(args[ARG_name].u_obj, &name_len);
    if (name_len < 1 || name_len > BTC_SERVICE_NAME_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("a name is 1 to 31 bytes"));
    }
    mp_int_t channel = -1, psm = -1;
    if (args[ARG_channel].u_obj != mp_const_none) {
        channel = mp_obj_get_int(args[ARG_channel].u_obj);
        if (channel < 1 || channel > 30) {
            mp_raise_ValueError(MP_ERROR_TEXT("channel is 1 to 30"));
        }
    }
    if (args[ARG_psm].u_obj != mp_const_none) {
        psm = mp_obj_get_int(args[ARG_psm].u_obj);
        if (psm < 1 || psm > 0xffff) {
            mp_raise_ValueError(MP_ERROR_TEXT("psm is 1 to 0xffff"));
        }
    }
    if (channel < 0 && psm < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("give a channel, a psm, or both"));
    }
    if (!btc_radio_active()) {
        int err = btc_radio_start();
        if (err < 0) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the radio didn't start"));
        }
    }
    // The name has to end in a NUL for the stack.
    char text[BTC_SERVICE_NAME_MAX + 1];
    memcpy(text, name, name_len);
    text[name_len] = 0;
    int slot = btc_service_add(&uuid, text, (int)channel, (int)psm);
    switch (slot) {
        case BTC_ERR_UNSUPPORTED:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: not in this firmware"));
        case BTC_ERR_NOMEM:
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: four services at most"));
        default:
            if (slot < 0) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("btclassic: the stack didn't make the record"));
            }
    }
    svc_obj_t *self = mp_obj_malloc(svc_obj_t, type);
    self->slot = slot;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t svc_deinit(mp_obj_t self_in) {
    svc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->slot >= 0) {
        btc_service_remove(self->slot);
        self->slot = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(svc_deinit_obj, svc_deinit);

static mp_obj_t svc_exit(size_t n_args, const mp_obj_t *args) {
    return svc_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(svc_exit_obj, 4, 4, svc_exit);

static void svc_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    svc_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr == MP_QSTR_active) {
        dest[0] = mp_obj_new_bool(self->slot >= 0 && btc_service_active(self->slot));
    } else {
        dest[1] = MP_OBJ_SENTINEL;  // methods, from the locals dict
    }
}

static const mp_rom_map_elem_t svc_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&svc_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&svc_exit_obj) },
};
static MP_DEFINE_CONST_DICT(svc_locals, svc_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    btc_service_type,
    MP_QSTR_Service,
    MP_TYPE_FLAG_NONE,
    make_new, svc_make_new,
    attr, svc_attr,
    locals_dict, &svc_locals
    );
