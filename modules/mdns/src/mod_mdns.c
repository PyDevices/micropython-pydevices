// mdns for MicroPython: CircuitPython's mdns API.
//
//   server = mdns.Server(wlan)              # or None
//   server.advertise_service(service_type="_http", protocol="_tcp", port=80,
//                            txt_records=["path=/"])
//   for s in server.find(service_type="_spotify-connect", protocol="_tcp"):
//       print(s.instance_name, s.ipv4_address, s.port)
//
// The names, defaults, limits and errors are CircuitPython's (shared-bindings/
// mdns and its ports' common-hal). Each port's work is in mdns_<port>.c,
// behind mdns_port.h.
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "mdns_port.h"

#if MDNS_AVAILABLE

void mdns_copy_str(char *dest, const char *src, size_t dest_len) {
    size_t n = src == NULL ? 0 : strlen(src);
    if (n >= dest_len) {
        n = dest_len - 1;
    }
    if (n) {
        memcpy(dest, src, n);
    }
    dest[n] = '\0';
}

// --- RemoteService -------------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    mdns_found_t found;
} mdns_remoteservice_obj_t;

static void mdns_remoteservice_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] != MP_OBJ_NULL) {
        return; // read-only
    }
    mdns_remoteservice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const mdns_found_t *f = &self->found;
    switch (attr) {
        case MP_QSTR_hostname:
            dest[0] = mp_obj_new_str(f->hostname, strlen(f->hostname));
            break;
        case MP_QSTR_instance_name:
            dest[0] = mp_obj_new_str(f->instance_name, strlen(f->instance_name));
            break;
        case MP_QSTR_service_type:
            dest[0] = mp_obj_new_str(f->service_type, strlen(f->service_type));
            break;
        case MP_QSTR_protocol:
            dest[0] = mp_obj_new_str(f->protocol, strlen(f->protocol));
            break;
        case MP_QSTR_port:
            dest[0] = MP_OBJ_NEW_SMALL_INT(f->port);
            break;
        case MP_QSTR_ipv4_address:
            // CircuitPython's is an ipaddress.IPv4Address, which MicroPython
            // doesn't have; str() of either is the dotted address.
            if (f->has_ipv4) {
                vstr_t v;
                vstr_init(&v, 16);
                vstr_printf(&v, "%u.%u.%u.%u", f->ipv4[0], f->ipv4[1], f->ipv4[2], f->ipv4[3]);
                dest[0] = mp_obj_new_str_from_vstr(&v);
            } else {
                dest[0] = mp_const_none;
            }
            break;
        default:
            break;
    }
}

static void mdns_remoteservice_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mdns_remoteservice_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<RemoteService %s.%s.%s port=%u>", self->found.instance_name,
        self->found.service_type, self->found.protocol, self->found.port);
}

static MP_DEFINE_CONST_OBJ_TYPE(
    mdns_remoteservice_type,
    MP_QSTR_RemoteService,
    MP_TYPE_FLAG_NONE,
    print, mdns_remoteservice_print,
    attr, mdns_remoteservice_attr
    );

// --- Server --------------------------------------------------------------------

// The live Server, if any. CircuitPython allows one at a time; deinit() (or
// the finaliser, which a soft reset runs) frees the place. Not a root pointer:
// an unreachable Server is collected and its finaliser clears this.
static mdns_server_obj_t *mdns_active;

static mp_obj_t mdns_server_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_network_interface };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_network_interface, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // In CircuitPython's order: the interface, then whether one is live.
    mdns_port_check_interface(args[ARG_network_interface].u_obj);
    if (mdns_active != NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("mDNS already initialized"));
    }
    mdns_server_obj_t *self = mp_obj_malloc_with_finaliser(mdns_server_obj_t, type);
    self->hostname[0] = '\0';
    self->instance_name[0] = '\0';
    self->num_advertised = 0;
    self->inited = false;
    mdns_port_construct(self, args[ARG_network_interface].u_obj);
    self->inited = true;
    mdns_active = self;
    return MP_OBJ_FROM_PTR(self);
}

static void check_for_deinit(mdns_server_obj_t *self) {
    if (!self->inited) {
        // CircuitPython's raise_deinited_error().
        mp_raise_ValueError(MP_ERROR_TEXT("Object has been deinitialized and can no longer be used. Create a new object."));
    }
}

static mp_obj_t mdns_server_deinit(mp_obj_t self_in) {
    mdns_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->inited) {
        return mp_const_none;
    }
    self->inited = false;
    if (mdns_active == self) {
        mdns_active = NULL;
    }
    mdns_port_deinit(self);
    self->num_advertised = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mdns_server_deinit_obj, mdns_server_deinit);

static mp_obj_t mdns_server_find(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mdns_server_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);

    // Keyword-only, as CircuitPython's binding has them.
    enum { ARG_service_type, ARG_protocol, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_service_type, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_protocol, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NEW_SMALL_INT(1)} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    #if MICROPY_PY_BUILTINS_FLOAT
    mp_float_t timeout = mp_obj_get_float(args[ARG_timeout].u_obj);
    mp_int_t timeout_ms = timeout <= 0 ? 0 : (mp_int_t)(timeout * 1000);
    #else
    mp_int_t timeout_ms = mp_obj_get_int(args[ARG_timeout].u_obj) * 1000;
    #endif
    const char *service_type = mp_obj_str_get_str(args[ARG_service_type].u_obj);
    const char *protocol = mp_obj_str_get_str(args[ARG_protocol].u_obj);

    mdns_found_t *found = m_new(mdns_found_t, MDNS_FIND_MAX);
    memset(found, 0, sizeof(mdns_found_t) * MDNS_FIND_MAX);
    size_t n = mdns_port_find(self, service_type, protocol, timeout_ms, found, MDNS_FIND_MAX);
    mp_obj_tuple_t *tuple = MP_OBJ_TO_PTR(mp_obj_new_tuple(n, NULL));
    for (size_t i = 0; i < n; i++) {
        mdns_remoteservice_obj_t *service = mp_obj_malloc(mdns_remoteservice_obj_t, &mdns_remoteservice_type);
        service->found = found[i];
        tuple->items[i] = MP_OBJ_FROM_PTR(service);
    }
    m_del(mdns_found_t, found, MDNS_FIND_MAX);
    return MP_OBJ_FROM_PTR(tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mdns_server_find_obj, 1, mdns_server_find);

static mp_obj_t mdns_server_advertise_service(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mdns_server_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    check_for_deinit(self);

    enum { ARG_service_type, ARG_protocol, ARG_port, ARG_txt_records };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_service_type, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_protocol, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_port, MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_txt_records, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const char *service_type = mp_obj_str_get_str(args[ARG_service_type].u_obj);
    const char *protocol = mp_obj_str_get_str(args[ARG_protocol].u_obj);
    mp_int_t port = args[ARG_port].u_int;
    if (port < 0 || port > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("port must be 0-65535"));
    }

    mp_obj_t txt_records = args[ARG_txt_records].u_obj;
    size_t num_txt = txt_records == mp_const_none ? 0 : (size_t)mp_obj_get_int(mp_obj_len(txt_records));
    if (num_txt > MDNS_MAX_TXT_RECORDS) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("%q length must be <= %d"),
            MP_QSTR_txt_records, MDNS_MAX_TXT_RECORDS);
    }
    const char *txt[MDNS_MAX_TXT_RECORDS];
    size_t txt_len[MDNS_MAX_TXT_RECORDS];
    for (size_t i = 0; i < num_txt; i++) {
        mp_obj_t record = mp_obj_subscr(txt_records, MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_SENTINEL);
        if (!mp_obj_is_str_or_bytes(record)) {
            mp_raise_ValueError(MP_ERROR_TEXT("Failed to add service TXT record; non-string or bytes found in txt_records"));
        }
        txt[i] = mp_obj_str_get_data(record, &txt_len[i]);
        if (txt_len[i] == 0 || txt_len[i] > 255 || txt[i][0] == '=') {
            mp_raise_ValueError(MP_ERROR_TEXT("Failed to add service TXT record"));
        }
    }

    // service_type and protocol occur on one port; a second call updates it.
    bool update = false;
    for (size_t i = 0; i < self->num_advertised; i++) {
        if (strcmp(self->advertised[i].service_type, service_type) == 0
            && strcmp(self->advertised[i].protocol, protocol) == 0) {
            update = true;
            break;
        }
    }
    if (!update && self->num_advertised == MDNS_MAX_SERVICES) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Out of MDNS service slots"));
    }
    mdns_port_advertise(self, update, service_type, protocol, (uint16_t)port, txt, txt_len, num_txt);
    if (!update) {
        mdns_advertised_t *a = &self->advertised[self->num_advertised++];
        mdns_copy_str(a->service_type, service_type, sizeof(a->service_type));
        mdns_copy_str(a->protocol, protocol, sizeof(a->protocol));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mdns_server_advertise_service_obj, 1, mdns_server_advertise_service);

static void mdns_server_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mdns_server_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (attr != MP_QSTR_hostname && attr != MP_QSTR_instance_name) {
        dest[1] = MP_OBJ_SENTINEL; // carry on to the methods
        return;
    }
    if (dest[0] == MP_OBJ_NULL) {
        check_for_deinit(self);
        const char *s = self->hostname;
        if (attr == MP_QSTR_instance_name && self->instance_name[0] != '\0') {
            s = self->instance_name;
        }
        dest[0] = mp_obj_new_str(s, strlen(s));
    } else if (dest[0] == MP_OBJ_SENTINEL && dest[1] != MP_OBJ_NULL) {
        check_for_deinit(self);
        const char *s = mp_obj_str_get_str(dest[1]);
        if (attr == MP_QSTR_hostname) {
            mdns_port_set_hostname(self, s);
            mdns_copy_str(self->hostname, s, sizeof(self->hostname));
        } else {
            mdns_port_set_instance_name(self, s);
            mdns_copy_str(self->instance_name, s, sizeof(self->instance_name));
        }
        dest[0] = MP_OBJ_NULL; // stored
    }
}

static const mp_rom_map_elem_t mdns_server_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_find), MP_ROM_PTR(&mdns_server_find_obj) },
    { MP_ROM_QSTR(MP_QSTR_advertise_service), MP_ROM_PTR(&mdns_server_advertise_service_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mdns_server_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&mdns_server_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(mdns_server_locals_dict, mdns_server_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    mdns_server_type,
    MP_QSTR_Server,
    MP_TYPE_FLAG_NONE,
    make_new, mdns_server_make_new,
    attr, mdns_server_attr,
    locals_dict, &mdns_server_locals_dict
    );

// --- the module ----------------------------------------------------------------

static const mp_rom_map_elem_t mdns_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_mdns) },
    { MP_ROM_QSTR(MP_QSTR_Server), MP_ROM_PTR(&mdns_server_type) },
    { MP_ROM_QSTR(MP_QSTR_RemoteService), MP_ROM_PTR(&mdns_remoteservice_type) },
};
static MP_DEFINE_CONST_DICT(mdns_module_globals, mdns_module_globals_table);

const mp_obj_module_t mdns_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mdns_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_mdns, mdns_user_cmodule);

#endif // MDNS_AVAILABLE
