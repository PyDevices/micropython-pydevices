// mdns: what mod_mdns.c (the Python API, CircuitPython's) asks of a port.
//
// One backend is compiled in, chosen here: ESP-IDF's mDNS component on esp32,
// lwIP's mDNS on rp2, the system's DNS-SD API on Windows, and a responder and
// querier of our own on unix. Every function is called with the GIL held, from
// the thread running Python, and may raise.
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "py/obj.h"

// The qstr scan sees the port's own definitions, not a module's, so the
// backend is chosen from those. micropython.mk compiles these files only on
// unix and windows, so of the make ports' CYW43 boards only rp2's get here.
#if defined(ESP_PLATFORM)
#define MDNS_PORT_ESP32 (1)
#elif defined(MICROPY_PY_NETWORK_CYW43) && MICROPY_PY_NETWORK_CYW43 && MICROPY_PY_LWIP
#define MDNS_PORT_LWIP (1)
#elif defined(_WIN32)
#define MDNS_PORT_WINDOWS (1)
#elif defined(__unix__) || defined(__APPLE__)
#define MDNS_PORT_POSIX (1)
#endif

#if MDNS_PORT_ESP32 || MDNS_PORT_WINDOWS || MDNS_PORT_LWIP || MDNS_PORT_POSIX
#define MDNS_AVAILABLE (1)
#else
#define MDNS_AVAILABLE (0)
#endif

// RFC 6762 Appendix A: a label is 63 bytes at most.
#define MDNS_NAME_MAX (64)
#define MDNS_PROTO_MAX (8)
// CircuitPython's own limits (its rp2 port).
#define MDNS_MAX_TXT_RECORDS (32)
#define MDNS_MAX_SERVICES (8)
// The most services one find() returns.
#define MDNS_FIND_MAX (32)

typedef struct {
    char instance_name[MDNS_NAME_MAX];
    char hostname[MDNS_NAME_MAX];
    char service_type[MDNS_NAME_MAX];
    char protocol[MDNS_PROTO_MAX];
    uint16_t port;
    bool has_ipv4;
    uint8_t ipv4[4];
} mdns_found_t;

typedef struct {
    char service_type[MDNS_NAME_MAX];
    char protocol[MDNS_PROTO_MAX];
} mdns_advertised_t;

typedef struct _mdns_server_obj_t {
    mp_obj_base_t base;
    char hostname[MDNS_NAME_MAX];
    // Empty means the hostname, as in CircuitPython.
    char instance_name[MDNS_NAME_MAX];
    mdns_advertised_t advertised[MDNS_MAX_SERVICES];
    size_t num_advertised;
    bool inited;
} mdns_server_obj_t;

// Raise ValueError for a network_interface the port can't use.
void mdns_port_check_interface(mp_obj_t network_interface);
// Start, on a network_interface already checked, and fill in self->hostname.
void mdns_port_construct(mdns_server_obj_t *self, mp_obj_t network_interface);
// Withdraw every service self->advertised names, and stop.
void mdns_port_deinit(mdns_server_obj_t *self);
void mdns_port_set_hostname(mdns_server_obj_t *self, const char *hostname);
void mdns_port_set_instance_name(mdns_server_obj_t *self, const char *instance_name);
// Up to out_len services that answered within timeout_ms; how many.
size_t mdns_port_find(mdns_server_obj_t *self, const char *service_type, const char *protocol,
    mp_int_t timeout_ms, mdns_found_t *out, size_t out_len);
// Add a service, or (update true) change the port and TXT records of one
// self->advertised already holds. txt[i] is txt_len[i] bytes, "key=value" or "key".
void mdns_port_advertise(mdns_server_obj_t *self, bool update, const char *service_type, const char *protocol,
    uint16_t port, const char **txt, const size_t *txt_len, size_t num_txt);

// Shared by the ports.
void mdns_copy_str(char *dest, const char *src, size_t dest_len);
