// mdns on esp32: ESP-IDF's mDNS component (espressif/mdns), the one the port
// already runs for <hostname>.local.
//
// The port starts it when the station or the LAN gets an address and records
// that in mdns_initialised (ports/esp32/network_wlan.c). A Server shares that
// flag: it starts the component only if the port hasn't, and never frees it,
// because the port's own name lookups (socket.getaddrinfo of a .local name)
// go on using it after a Server is gone. deinit() withdraws the Server's
// services and puts back the port's hostname.
//
// SPDX-License-Identifier: MIT

#include "mdns_port.h"

#if MDNS_PORT_ESP32

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "py/runtime.h"

#include "esp_err.h"
#include "mdns.h"

#if MICROPY_PY_NETWORK
extern char mod_network_hostname_data[];
#define PORT_HOSTNAME (mod_network_hostname_data)
#else
#define PORT_HOSTNAME ("mpy-esp32")
#endif

#if MICROPY_PY_NETWORK_WLAN && (MICROPY_HW_ENABLE_MDNS_QUERIES || MICROPY_HW_ENABLE_MDNS_RESPONDER)
extern bool mdns_initialised; // network_wlan.c's, which network_lan.c shares too
#else
static bool mdns_initialised = false;
#endif

static void mdns_esp_check(esp_err_t err) {
    if (err != ESP_OK) {
        mp_raise_OSError(err == ESP_ERR_NO_MEM ? MP_ENOMEM : MP_EIO);
    }
}

static void mdns_esp_start(void) {
    if (mdns_initialised) {
        return;
    }
    esp_err_t err = mdns_init();
    // Already started by someone else (an app's own component): share it.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        mdns_esp_check(err);
    }
    if (err == ESP_OK) {
        mdns_hostname_set(PORT_HOSTNAME);
        mdns_instance_name_set(PORT_HOSTNAME);
    }
    mdns_initialised = true;
}

void mdns_port_check_interface(mp_obj_t network_interface) {
    // network.WLAN or network.LAN; None is whichever has an address.
    if (network_interface != mp_const_none) {
        qstr name = mp_obj_get_type(network_interface)->name;
        if (name != MP_QSTR_WLAN && name != MP_QSTR_LAN) {
            mp_raise_ValueError(MP_ERROR_TEXT("mDNS only works with built-in WiFi"));
        }
    }
}

void mdns_port_construct(mdns_server_obj_t *self, mp_obj_t network_interface) {
    (void)network_interface;
    mdns_esp_start();
    char hostname[MDNS_NAME_MAX];
    if (mdns_hostname_get(hostname) != ESP_OK || hostname[0] == '\0') {
        mdns_copy_str(hostname, PORT_HOSTNAME, sizeof(hostname));
    }
    mdns_copy_str(self->hostname, hostname, sizeof(self->hostname));
}

void mdns_port_deinit(mdns_server_obj_t *self) {
    for (size_t i = 0; i < self->num_advertised; i++) {
        mdns_service_remove(self->advertised[i].service_type, self->advertised[i].protocol);
    }
    // Put back what the port set, if this Server changed it.
    if (strcmp(self->hostname, PORT_HOSTNAME) != 0) {
        mdns_hostname_set(PORT_HOSTNAME);
    }
    if (self->instance_name[0] != '\0') {
        mdns_instance_name_set(PORT_HOSTNAME);
    }
}

void mdns_port_set_hostname(mdns_server_obj_t *self, const char *hostname) {
    (void)self;
    mdns_esp_check(mdns_hostname_set(hostname));
    // The component's task applies it; wait for that, as CircuitPython does.
    for (int i = 0; i < 100 && !mdns_hostname_exists(hostname); i++) {
        MP_THREAD_GIL_EXIT();
        mp_hal_delay_us(10000);
        MP_THREAD_GIL_ENTER();
    }
}

void mdns_port_set_instance_name(mdns_server_obj_t *self, const char *instance_name) {
    (void)self;
    mdns_esp_check(mdns_instance_name_set(instance_name));
}

static void copy_result(const mdns_result_t *r, mdns_found_t *out) {
    mdns_copy_str(out->instance_name, r->instance_name, sizeof(out->instance_name));
    mdns_copy_str(out->hostname, r->hostname, sizeof(out->hostname));
    mdns_copy_str(out->service_type, r->service_type, sizeof(out->service_type));
    mdns_copy_str(out->protocol, r->proto, sizeof(out->protocol));
    out->port = r->port;
    out->has_ipv4 = false;
    for (const mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            memcpy(out->ipv4, &a->addr.u_addr.ip4.addr, 4); // network order
            out->has_ipv4 = true;
            break;
        }
    }
}

size_t mdns_port_find(mdns_server_obj_t *self, const char *service_type, const char *protocol,
    mp_int_t timeout_ms, mdns_found_t *out, size_t out_len) {
    (void)self;
    mdns_search_once_t *search = mdns_query_async_new(NULL, service_type, protocol, MDNS_TYPE_PTR,
        timeout_ms, 255, NULL);
    if (search == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Unable to start mDNS query"));
    }
    mdns_result_t *results = NULL;
    uint8_t num_results = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        for (;;) {
            bool done;
            MP_THREAD_GIL_EXIT();
            done = mdns_query_async_get_results(search, 10, &results, &num_results);
            MP_THREAD_GIL_ENTER();
            if (done) {
                break;
            }
            mp_handle_pending(true); // Ctrl-C ends the search
        }
        nlr_pop();
    } else {
        mdns_query_async_delete(search);
        nlr_jump(nlr.ret_val);
    }
    mdns_query_async_delete(search);
    size_t n = 0;
    for (mdns_result_t *r = results; r != NULL && n < out_len; r = r->next) {
        copy_result(r, &out[n++]);
    }
    mdns_query_results_free(results);
    return n;
}

static void set_txt(const char *service_type, const char *protocol, const char **txt, const size_t *txt_len, size_t num_txt) {
    for (size_t i = 0; i < num_txt; i++) {
        // "key=value", or a key alone (a boolean attribute, RFC 6763 6.4).
        char key[256];
        size_t k = 0;
        while (k < txt_len[i] && txt[i][k] != '=') {
            k++;
        }
        memcpy(key, txt[i], k);
        key[k] = '\0';
        const char *value = k < txt_len[i] ? txt[i] + k + 1 : NULL;
        size_t value_len = k < txt_len[i] ? txt_len[i] - k - 1 : 0;
        esp_err_t err = mdns_service_txt_item_set_with_explicit_value_len(service_type, protocol, key,
            value_len ? value : NULL, (uint8_t)value_len);
        if (err != ESP_OK) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Failed to add service TXT record"));
        }
    }
}

void mdns_port_advertise(mdns_server_obj_t *self, bool update, const char *service_type, const char *protocol,
    uint16_t port, const char **txt, const size_t *txt_len, size_t num_txt) {
    (void)self;
    (void)update;
    if (mdns_service_exists(service_type, protocol, NULL)) {
        // Ours from an earlier call, or one the firmware added: either way
        // this call's port and records replace it.
        mdns_esp_check(mdns_service_port_set(service_type, protocol, port));
        mdns_esp_check(mdns_service_txt_set(service_type, protocol, NULL, 0));
    } else {
        // NULL: the service takes the Server's instance_name.
        mdns_esp_check(mdns_service_add(NULL, service_type, protocol, port, NULL, 0));
    }
    set_txt(service_type, protocol, txt, txt_len, num_txt);
}

#endif // MDNS_PORT_ESP32
