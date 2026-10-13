// mdns on rp2 boards with a CYW43 radio (the Pico W and Pico 2 W): lwIP's
// mDNS responder and its search, as CircuitPython's raspberrypi port uses
// them.
//
// The port starts lwIP's responder (mdns_resp_init() in main.c) but gives it
// no interface; a Server adds the station's, under the port's hostname, and
// takes it off again at deinit(). lwIP's default allows one service
// (MDNS_MAX_SERVICES) and two searches at a time.
//
// lwIP calls back from its own context, so the callbacks write only to state
// here, never to the MicroPython heap, and every lwIP call holds its lock.
//
// SPDX-License-Identifier: MIT

#include "mdns_port.h"

#if MDNS_PORT_LWIP

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"

#include "lib/cyw43-driver/src/cyw43.h"
#include "lwip/apps/mdns.h"
#include "lwip/prot/dns.h"

extern char mod_network_hostname_data[];

#define NETIF_STA (&cyw43_state.netif[CYW43_ITF_STA])

typedef struct {
    char type[MDNS_NAME_MAX];
    char proto[MDNS_PROTO_MAX];
    uint16_t port;
    s8_t slot; // lwIP's, or -1
    size_t num_txt;
    uint8_t txt_len[MDNS_MAX_TXT_RECORDS];
    char txt[MDNS_MAX_TXT_RECORDS][256];
} svc_t;

static struct {
    char instance[MDNS_NAME_MAX];
    bool added_netif; // we gave lwIP the station's interface
    svc_t svcs[MDNS_MAX_SERVICES];
} g;

void mdns_port_check_interface(mp_obj_t network_interface) {
    if (network_interface != mp_const_none && mp_obj_get_type(network_interface)->name != MP_QSTR_WLAN) {
        mp_raise_ValueError(MP_ERROR_TEXT("mDNS only works with built-in WiFi"));
    }
}

void mdns_port_construct(mdns_server_obj_t *self, mp_obj_t network_interface) {
    (void)network_interface;
    memset(&g, 0, sizeof(g));
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        g.svcs[i].slot = -1;
    }
    MICROPY_PY_LWIP_ENTER
    err_t err = ERR_OK;
    if (!mdns_resp_netif_active(NETIF_STA)) {
        err = mdns_resp_add_netif(NETIF_STA, mod_network_hostname_data);
        g.added_netif = err == ERR_OK;
    }
    MICROPY_PY_LWIP_EXIT
    if (err != ERR_OK) {
        mp_raise_OSError(MP_ENOMEM);
    }
    mdns_copy_str(self->hostname, mod_network_hostname_data, sizeof(self->hostname));
    mdns_copy_str(g.instance, mod_network_hostname_data, sizeof(g.instance));
}

static void withdraw_all(void) {
    MICROPY_PY_LWIP_ENTER
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        if (g.svcs[i].slot >= 0) {
            mdns_resp_del_service(NETIF_STA, (u8_t)g.svcs[i].slot);
            g.svcs[i].slot = -1;
        }
    }
    MICROPY_PY_LWIP_EXIT
}

void mdns_port_deinit(mdns_server_obj_t *self) {
    (void)self;
    withdraw_all();
    memset(g.svcs, 0, sizeof(g.svcs));
    MICROPY_PY_LWIP_ENTER
    if (g.added_netif) {
        mdns_resp_remove_netif(NETIF_STA);
        g.added_netif = false;
    }
    MICROPY_PY_LWIP_EXIT
}

static void srv_txt(struct mdns_service *service, void *arg) {
    const svc_t *s = arg;
    for (size_t i = 0; i < s->num_txt; i++) {
        mdns_resp_add_service_txtitem(service, s->txt[i], s->txt_len[i]);
    }
}

static enum mdns_sd_proto proto_of(const char *protocol) {
    return strcmp(protocol, "_tcp") == 0 ? DNSSD_PROTO_TCP : DNSSD_PROTO_UDP;
}

// Give s to lwIP under the current instance name.
static void publish(svc_t *s) {
    MICROPY_PY_LWIP_ENTER
    s->slot = mdns_resp_add_service(NETIF_STA, g.instance, s->type, proto_of(s->proto), s->port, srv_txt, s);
    MICROPY_PY_LWIP_EXIT
    if (s->slot < 0) {
        s->slot = -1;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Out of MDNS service slots"));
    }
}

void mdns_port_set_hostname(mdns_server_obj_t *self, const char *hostname) {
    (void)self;
    MICROPY_PY_LWIP_ENTER
    err_t err = mdns_resp_rename_netif(NETIF_STA, hostname);
    MICROPY_PY_LWIP_EXIT
    if (err != ERR_OK) {
        mp_raise_ValueError(MP_ERROR_TEXT("hostname"));
    }
}

void mdns_port_set_instance_name(mdns_server_obj_t *self, const char *instance_name) {
    (void)self;
    mdns_copy_str(g.instance, instance_name, sizeof(g.instance));
    // lwIP keeps the name per service: offer each again under the new one.
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        svc_t *s = &g.svcs[i];
        if (s->slot >= 0) {
            MICROPY_PY_LWIP_ENTER
            mdns_resp_del_service(NETIF_STA, (u8_t)s->slot);
            MICROPY_PY_LWIP_EXIT
            s->slot = -1;
            publish(s);
        }
    }
}

void mdns_port_advertise(mdns_server_obj_t *self, bool update, const char *service_type, const char *protocol,
    uint16_t port, const char **txt, const size_t *txt_len, size_t num_txt) {
    (void)self;
    (void)update;
    svc_t *s = NULL;
    for (size_t i = 0; i < MDNS_MAX_SERVICES && !s; i++) {
        if (g.svcs[i].slot >= 0 && strcmp(g.svcs[i].type, service_type) == 0 && strcmp(g.svcs[i].proto, protocol) == 0) {
            s = &g.svcs[i];
        }
    }
    for (size_t i = 0; i < MDNS_MAX_SERVICES && !s; i++) {
        if (g.svcs[i].slot < 0) {
            s = &g.svcs[i];
        }
    }
    if (s == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Out of MDNS service slots"));
    }
    // Remove the existing service first, as CircuitPython does.
    if (s->slot >= 0) {
        MICROPY_PY_LWIP_ENTER
        mdns_resp_del_service(NETIF_STA, (u8_t)s->slot);
        MICROPY_PY_LWIP_EXIT
        s->slot = -1;
    }
    mdns_copy_str(s->type, service_type, sizeof(s->type));
    mdns_copy_str(s->proto, protocol, sizeof(s->proto));
    s->port = port;
    s->num_txt = num_txt;
    for (size_t i = 0; i < num_txt; i++) {
        memcpy(s->txt[i], txt[i], txt_len[i]);
        s->txt_len[i] = (uint8_t)txt_len[i];
    }
    publish(s);
}

// --- find() --------------------------------------------------------------------

static struct {
    mdns_found_t *out;
    size_t out_len;
    size_t n;
    volatile u8_t request_id;
} search;

static void copy_label(char *dest, size_t dest_len, const u8_t *src, size_t len) {
    if (len >= dest_len) {
        len = dest_len - 1;
    }
    memcpy(dest, src, len);
    dest[len] = '\0';
}

// One answering service per frame: its SRV names it and gives the port, its
// A record the host and address (CircuitPython's reading of lwIP's search).
static void search_result(struct mdns_answer *answer, const char *varpart, int varlen, int flags, void *arg) {
    (void)arg;
    if ((flags & MDNS_SEARCH_RESULT_FIRST) && search.n < search.out_len) {
        memset(&search.out[search.n], 0, sizeof(mdns_found_t));
        search.n++;
    }
    if (search.n == 0 || varlen <= 0) {
        return;
    }
    mdns_found_t *f = &search.out[search.n - 1];
    const u8_t *d = answer->info.domain.name;
    if (answer->info.type == DNS_RRTYPE_A && varlen >= 4) {
        copy_label(f->hostname, sizeof(f->hostname), d + 1, d[0]);
        memcpy(f->ipv4, varpart, 4);
        f->has_ipv4 = true;
    } else if (answer->info.type == DNS_RRTYPE_SRV && varlen >= 6) {
        size_t o = 0;
        copy_label(f->instance_name, sizeof(f->instance_name), d + o + 1, d[o]);
        o += 1 + d[o];
        copy_label(f->service_type, sizeof(f->service_type), d + o + 1, d[o]);
        o += 1 + d[o];
        copy_label(f->protocol, sizeof(f->protocol), d + o + 1, d[o]);
        f->port = (uint16_t)(((u8_t)varpart[4] << 8) | (u8_t)varpart[5]);
    }
}

size_t mdns_port_find(mdns_server_obj_t *self, const char *service_type, const char *protocol,
    mp_int_t timeout_ms, mdns_found_t *out, size_t out_len) {
    (void)self;
    search.out = out;
    search.out_len = out_len;
    search.n = 0;
    u8_t request_id;
    MICROPY_PY_LWIP_ENTER
    err_t err = mdns_search_service(NULL, service_type, proto_of(protocol), NETIF_STA, search_result, NULL, &request_id);
    MICROPY_PY_LWIP_EXIT
    if (err != ERR_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Unable to start mDNS query"));
    }
    nlr_buf_t nlr;
    mp_uint_t start = mp_hal_ticks_ms();
    if (nlr_push(&nlr) == 0) {
        while ((mp_int_t)(mp_hal_ticks_ms() - start) < timeout_ms) {
            mp_event_wait_ms(10); // runs lwIP and raises a Ctrl-C
        }
        nlr_pop();
    } else {
        MICROPY_PY_LWIP_ENTER
        mdns_search_stop(request_id);
        MICROPY_PY_LWIP_EXIT
        nlr_jump(nlr.ret_val);
    }
    MICROPY_PY_LWIP_ENTER
    mdns_search_stop(request_id);
    MICROPY_PY_LWIP_EXIT
    // Each service as asked for, even where a frame left it unnamed.
    for (size_t i = 0; i < search.n; i++) {
        mdns_copy_str(out[i].service_type, service_type, sizeof(out[i].service_type));
        mdns_copy_str(out[i].protocol, protocol, sizeof(out[i].protocol));
    }
    return search.n;
}

#endif // MDNS_PORT_LWIP
