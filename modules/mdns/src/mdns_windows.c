// mdns on Windows: the system's DNS-SD API (windns.h, Windows 10 and later).
//
// Windows' own mDNS service holds UDP 5353, so a program doesn't answer
// questions itself: DnsServiceRegister() hands a service to that service,
// which answers for it and for its host name, and DnsServiceBrowse() and
// DnsServiceResolve() ask through it.
//
// The API calls back on a thread of its own. Callbacks touch only a context
// allocated with malloc, under its critical section, never the MicroPython
// heap; the context is freed by whichever side lets go of it last.
//
// SPDX-License-Identifier: MIT

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#elif _WIN32_WINNT < 0x0A00
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
#endif

#include "mdns_port.h"

#if MDNS_PORT_WINDOWS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "py/runtime.h"

// --- shared helpers --------------------------------------------------------------

static void wsa_start(void) {
    static bool started = false;
    if (!started) {
        WSADATA d;
        started = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
}

static bool to_wide(const char *s, wchar_t *out, size_t cap) {
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap) > 0;
}

static void from_wide(const wchar_t *s, char *out, size_t cap) {
    if (s == NULL || WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)cap, NULL, NULL) <= 0) {
        out[0] = '\0';
    }
}

// The IPv4 address multicast leaves from (as on unix: the route to the mDNS
// group, or the default route).
static bool local_ipv4(uint8_t ip[4]) {
    static const char *const via[] = { "224.0.0.251", "8.8.8.8" };
    wsa_start();
    for (size_t i = 0; i < 2; i++) {
        SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd == INVALID_SOCKET) {
            return false;
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(5353);
        inet_pton(AF_INET, via[i], &a.sin_addr);
        int alen = sizeof(a);
        bool ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0
            && getsockname(fd, (struct sockaddr *)&a, &alen) == 0
            && a.sin_addr.s_addr != htonl(INADDR_ANY);
        closesocket(fd);
        if (ok) {
            memcpy(ip, &a.sin_addr.s_addr, 4);
            return true;
        }
    }
    return false;
}

// Wait up to ms for *flag, letting Python run (and Ctrl-C raise) meanwhile.
static void wait_for(volatile LONG *flag, mp_int_t ms) {
    mp_uint_t start = mp_hal_ticks_ms();
    while (!*flag && (mp_int_t)(mp_hal_ticks_ms() - start) < ms) {
        MP_THREAD_GIL_EXIT();
        Sleep(10);
        MP_THREAD_GIL_ENTER();
        mp_handle_pending(true);
    }
}

// --- advertising -----------------------------------------------------------------

typedef struct {
    DNS_SERVICE_REGISTER_REQUEST req;
    PDNS_SERVICE_INSTANCE inst;
    volatile LONG done;
    volatile DWORD status;
    char type[MDNS_NAME_MAX];
    char proto[MDNS_PROTO_MAX];
    uint16_t port;
    size_t num_txt;
    char *txt; // num_txt NUL-terminated records, one after another
} reg_t;

static struct {
    uint8_t ip[4];
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];
    reg_t *regs[MDNS_MAX_SERVICES];
} g;

static VOID WINAPI register_done(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    reg_t *r = ctx;
    r->status = status;
    if (inst != NULL) {
        DnsServiceFreeInstance(inst);
    }
    InterlockedExchange(&r->done, 1);
}

static void reg_free(reg_t *r) {
    if (r->inst != NULL) {
        DnsServiceFreeInstance(r->inst);
    }
    free(r->txt);
    free(r);
}

static void withdraw(reg_t *r) {
    InterlockedExchange(&r->done, 0);
    if (DnsServiceDeRegister(&r->req, NULL) == DNS_REQUEST_PENDING) {
        wait_for(&r->done, 2000);
    }
    if (r->done) {
        reg_free(r);
    }
    // else: Windows still holds r, and calls back into it later; leave it.
}

// Hand r to Windows' mDNS service, built from the computer's name, the
// instance name and the address; raises if Windows refuses it.
static void publish(reg_t *r) {
    char name[3 * MDNS_NAME_MAX + 16], host[MDNS_NAME_MAX + 8];
    wchar_t wname[3 * MDNS_NAME_MAX + 16], whost[MDNS_NAME_MAX + 8];
    snprintf(name, sizeof(name), "%s.%s.%s.local", g.instance, r->type, r->proto);
    snprintf(host, sizeof(host), "%s.local", g.host);
    wchar_t *keys[MDNS_MAX_TXT_RECORDS], *values[MDNS_MAX_TXT_RECORDS];
    wchar_t wtxt[MDNS_MAX_TXT_RECORDS][260];
    const char *t = r->txt;
    for (size_t i = 0; i < r->num_txt; i++) {
        size_t n = strlen(t);
        to_wide(t, wtxt[i], 260);
        wchar_t *eq = wcschr(wtxt[i], L'=');
        keys[i] = wtxt[i];
        if (eq) {
            *eq = 0;
            values[i] = eq + 1;
        } else {
            values[i] = wtxt[i] + wcslen(wtxt[i]); // ""
        }
        t += n + 1;
    }
    if (!to_wide(name, wname, MP_ARRAY_SIZE(wname)) || !to_wide(host, whost, MP_ARRAY_SIZE(whost))) {
        mp_raise_ValueError(MP_ERROR_TEXT("Failed to add service"));
    }
    IP4_ADDRESS ip4;
    memcpy(&ip4, g.ip, 4);
    r->inst = DnsServiceConstructInstance(wname, whost, &ip4, NULL, r->port, 0, 0,
        (DWORD)r->num_txt, (PCWSTR *)keys, (PCWSTR *)values);
    if (r->inst == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }
    memset(&r->req, 0, sizeof(r->req));
    r->req.Version = DNS_QUERY_REQUEST_VERSION1;
    r->req.InterfaceIndex = 0;
    r->req.pServiceInstance = r->inst;
    r->req.pRegisterCompletionCallback = register_done;
    r->req.pQueryContext = r;
    r->req.unicastEnabled = FALSE;
    InterlockedExchange(&r->done, 0);
    r->status = 0;
    DWORD rc = DnsServiceRegister(&r->req, NULL);
    if (rc != DNS_REQUEST_PENDING) {
        mp_raise_OSError(rc == ERROR_SUCCESS ? MP_EIO : (int)rc);
    }
    // Windows probes the name first, which takes about a second.
    wait_for(&r->done, 5000);
    if (r->done && r->status != ERROR_SUCCESS) {
        mp_raise_OSError((int)r->status);
    }
}

// Withdraw every service and publish it again, as the instance name changed.
static void republish_all(void) {
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        reg_t *r = g.regs[i];
        if (r == NULL) {
            continue;
        }
        reg_t *fresh = calloc(1, sizeof(reg_t));
        if (fresh == NULL) {
            mp_raise_OSError(MP_ENOMEM);
        }
        *fresh = *r;
        fresh->inst = NULL;
        r->txt = NULL; // fresh owns it now
        withdraw(r);
        g.regs[i] = fresh;
        publish(fresh);
    }
}

void mdns_port_check_interface(mp_obj_t network_interface) {
    // None, or on a computer with more than one network the IPv4 address to
    // work from.
    struct in_addr a;
    if (network_interface != mp_const_none
        && (!mp_obj_is_str(network_interface)
            || inet_pton(AF_INET, mp_obj_str_get_str(network_interface), &a) != 1)) {
        mp_raise_ValueError(MP_ERROR_TEXT("network_interface must be None or an IPv4 address"));
    }
}

void mdns_port_construct(mdns_server_obj_t *self, mp_obj_t network_interface) {
    uint8_t ip[4];
    wsa_start();
    if (network_interface == mp_const_none) {
        if (!local_ipv4(ip)) {
            mp_raise_OSError(MP_EHOSTUNREACH);
        }
    } else if (mp_obj_is_str(network_interface)) {
        struct in_addr a;
        if (inet_pton(AF_INET, mp_obj_str_get_str(network_interface), &a) != 1) {
            mp_raise_ValueError(MP_ERROR_TEXT("network_interface must be None or an IPv4 address"));
        }
        memcpy(ip, &a.s_addr, 4);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("network_interface must be None or an IPv4 address"));
    }
    char host[MDNS_NAME_MAX];
    DWORD n = sizeof(host);
    if (!GetComputerNameExA(ComputerNameDnsHostname, host, &n) || host[0] == '\0') {
        strcpy(host, "micropython");
    }
    memcpy(g.ip, ip, 4);
    mdns_copy_str(g.host, host, sizeof(g.host));
    mdns_copy_str(g.instance, host, sizeof(g.instance));
    memset(g.regs, 0, sizeof(g.regs));
    mdns_copy_str(self->hostname, host, sizeof(self->hostname));
}

void mdns_port_deinit(mdns_server_obj_t *self) {
    (void)self;
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        if (g.regs[i] != NULL) {
            reg_t *r = g.regs[i];
            g.regs[i] = NULL;
            withdraw(r);
        }
    }
}

void mdns_port_set_hostname(mdns_server_obj_t *self, const char *hostname) {
    (void)self;
    // Windows' mDNS service answers for the computer's own name only: given
    // any other, it announces services whose host has no address, and
    // ESP-IDF's querier then drops the whole answer, other services and all.
    if (_stricmp(hostname, g.host) != 0) {
        mp_raise_msg(&mp_type_NotImplementedError,
            MP_ERROR_TEXT("on Windows the hostname is the computer's name"));
    }
}

void mdns_port_set_instance_name(mdns_server_obj_t *self, const char *instance_name) {
    (void)self;
    mdns_copy_str(g.instance, instance_name, sizeof(g.instance));
    republish_all();
}

void mdns_port_advertise(mdns_server_obj_t *self, bool update, const char *service_type, const char *protocol,
    uint16_t port, const char **txt, const size_t *txt_len, size_t num_txt) {
    (void)self;
    (void)update;
    reg_t *r = calloc(1, sizeof(reg_t));
    size_t total = 0;
    for (size_t i = 0; i < num_txt; i++) {
        total += txt_len[i] + 1;
    }
    char *records = malloc(total ? total : 1);
    if (r == NULL || records == NULL) {
        free(r);
        free(records);
        mp_raise_OSError(MP_ENOMEM);
    }
    char *p = records;
    for (size_t i = 0; i < num_txt; i++) {
        memcpy(p, txt[i], txt_len[i]);
        p[txt_len[i]] = '\0';
        p += txt_len[i] + 1;
    }
    r->txt = records;
    r->num_txt = num_txt;
    mdns_copy_str(r->type, service_type, sizeof(r->type));
    mdns_copy_str(r->proto, protocol, sizeof(r->proto));
    r->port = port;
    // A second call for the same service replaces it.
    size_t slot = MDNS_MAX_SERVICES;
    for (size_t i = 0; i < MDNS_MAX_SERVICES; i++) {
        reg_t *o = g.regs[i];
        if (o != NULL && strcmp(o->type, service_type) == 0 && strcmp(o->proto, protocol) == 0) {
            g.regs[i] = NULL;
            withdraw(o);
            slot = i;
            break;
        }
    }
    for (size_t i = 0; i < MDNS_MAX_SERVICES && slot == MDNS_MAX_SERVICES; i++) {
        if (g.regs[i] == NULL) {
            slot = i;
        }
    }
    if (slot == MDNS_MAX_SERVICES) {
        reg_free(r);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Out of MDNS service slots"));
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        publish(r);
        nlr_pop();
    } else {
        if (r->done) {
            reg_free(r);
        }
        nlr_jump(nlr.ret_val);
    }
    g.regs[slot] = r;
}

// --- find(): browse, then resolve ----------------------------------------------------

typedef struct {
    char full[256]; // "<instance>.<service>.local"
    char host[256];
    uint16_t port;
    bool has_ipv4;
    uint8_t ipv4[4];
    bool resolved;
    bool resolving;
    wchar_t wfull[256];
    DNS_SERVICE_RESOLVE_REQUEST req;
    DNS_SERVICE_CANCEL cancel;
} hit_t;

typedef struct {
    CRITICAL_SECTION cs;
    volatile LONG refs; // ours, plus one per request Windows still holds
    hit_t hits[MDNS_FIND_MAX];
    size_t n;
    wchar_t query[300];
    DNS_SERVICE_BROWSE_REQUEST req;
    DNS_SERVICE_CANCEL cancel;
} browse_t;

typedef struct {
    browse_t *b;
    size_t i;
} resolve_ctx_t;

static void browse_release(browse_t *b) {
    if (InterlockedDecrement(&b->refs) == 0) {
        DeleteCriticalSection(&b->cs);
        free(b);
    }
}

static hit_t *hit_for(browse_t *b, const char *full) {
    for (size_t i = 0; i < b->n; i++) {
        if (_stricmp(b->hits[i].full, full) == 0) {
            return &b->hits[i];
        }
    }
    if (b->n == MDNS_FIND_MAX) {
        return NULL;
    }
    hit_t *h = &b->hits[b->n++];
    memset(h, 0, sizeof(*h));
    mdns_copy_str(h->full, full, sizeof(h->full));
    return h;
}

static VOID WINAPI browse_cb(DWORD status, PVOID ctx, PDNS_RECORD records) {
    browse_t *b = ctx;
    if (status == ERROR_SUCCESS) {
        EnterCriticalSection(&b->cs);
        char name[256], target[256];
        // Some responders put the port and the address in the same answer.
        for (PDNS_RECORDW r = (PDNS_RECORDW)records; r != NULL; r = r->pNext) {
            if (r->wType == DNS_TYPE_PTR) {
                from_wide(r->Data.PTR.pNameHost, target, sizeof(target));
                if (target[0]) {
                    hit_for(b, target);
                }
            }
        }
        for (PDNS_RECORDW r = (PDNS_RECORDW)records; r != NULL; r = r->pNext) {
            if (r->wType == DNS_TYPE_SRV) {
                from_wide(r->pName, name, sizeof(name));
                for (size_t i = 0; i < b->n; i++) {
                    if (_stricmp(b->hits[i].full, name) == 0) {
                        from_wide(r->Data.SRV.pNameTarget, b->hits[i].host, sizeof(b->hits[i].host));
                        b->hits[i].port = r->Data.SRV.wPort;
                    }
                }
            }
        }
        for (PDNS_RECORDW r = (PDNS_RECORDW)records; r != NULL; r = r->pNext) {
            if (r->wType == DNS_TYPE_A) {
                from_wide(r->pName, name, sizeof(name));
                for (size_t i = 0; i < b->n; i++) {
                    if (b->hits[i].host[0] && _stricmp(b->hits[i].host, name) == 0) {
                        memcpy(b->hits[i].ipv4, &r->Data.A.IpAddress, 4);
                        b->hits[i].has_ipv4 = true;
                        b->hits[i].resolved = true;
                    }
                }
            }
        }
        LeaveCriticalSection(&b->cs);
    }
    if (records != NULL) {
        DnsRecordListFree(records, DnsFreeRecordList);
    }
    if (status != ERROR_SUCCESS) {
        // The browse has ended (cancelled, or failed): Windows lets go of b.
        browse_release(b);
    }
}

static VOID WINAPI resolve_cb(DWORD status, PVOID ctx, PDNS_SERVICE_INSTANCE inst) {
    resolve_ctx_t *rc = ctx;
    browse_t *b = rc->b;
    EnterCriticalSection(&b->cs);
    hit_t *h = &b->hits[rc->i];
    if (status == ERROR_SUCCESS && inst != NULL) {
        from_wide(inst->pszHostName, h->host, sizeof(h->host));
        h->port = inst->wPort;
        if (inst->ip4Address != NULL) {
            memcpy(h->ipv4, inst->ip4Address, 4);
            h->has_ipv4 = true;
        }
        h->resolved = true;
    }
    h->resolving = false;
    LeaveCriticalSection(&b->cs);
    if (inst != NULL) {
        DnsServiceFreeInstance(inst);
    }
    free(rc);
    browse_release(b);
}

static void strip_suffix(char *name, const char *suffix) {
    size_t n = strlen(name), s = strlen(suffix);
    if (n > s && _stricmp(name + n - s, suffix) == 0) {
        name[n - s] = '\0';
    }
}

size_t mdns_port_find(mdns_server_obj_t *self, const char *service_type, const char *protocol,
    mp_int_t timeout_ms, mdns_found_t *out, size_t out_len) {
    (void)self;
    browse_t *b = calloc(1, sizeof(browse_t));
    if (b == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }
    InitializeCriticalSection(&b->cs);
    b->refs = 1;
    char svc[200];
    snprintf(svc, sizeof(svc), "%s.%s.local", service_type, protocol);
    to_wide(svc, b->query, MP_ARRAY_SIZE(b->query));

    b->req.Version = DNS_QUERY_REQUEST_VERSION1;
    b->req.InterfaceIndex = 0;
    b->req.QueryName = b->query;
    b->req.pBrowseCallback = browse_cb;
    b->req.pQueryContext = b;
    InterlockedIncrement(&b->refs);
    DNS_STATUS st = DnsServiceBrowse(&b->req, &b->cancel);
    if (st != DNS_REQUEST_PENDING) {
        browse_release(b); // Windows didn't take it
        browse_release(b);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Unable to start mDNS query"));
    }

    nlr_buf_t nlr;
    bool raised = false;
    void *exc = NULL;
    if (nlr_push(&nlr) == 0) {
        // Resolve each instance as it turns up, until the time is gone.
        mp_uint_t start = mp_hal_ticks_ms();
        while ((mp_int_t)(mp_hal_ticks_ms() - start) < timeout_ms) {
            EnterCriticalSection(&b->cs);
            for (size_t i = 0; i < b->n; i++) {
                hit_t *h = &b->hits[i];
                if (h->resolved || h->resolving) {
                    continue;
                }
                resolve_ctx_t *rc = malloc(sizeof(resolve_ctx_t));
                if (rc == NULL) {
                    continue;
                }
                rc->b = b;
                rc->i = i;
                to_wide(h->full, h->wfull, MP_ARRAY_SIZE(h->wfull));
                memset(&h->req, 0, sizeof(h->req));
                h->req.Version = DNS_QUERY_REQUEST_VERSION1;
                h->req.InterfaceIndex = 0;
                h->req.QueryName = h->wfull;
                h->req.pResolveCompletionCallback = resolve_cb;
                h->req.pQueryContext = rc;
                InterlockedIncrement(&b->refs);
                if (DnsServiceResolve(&h->req, &h->cancel) == DNS_REQUEST_PENDING) {
                    h->resolving = true;
                } else {
                    InterlockedDecrement(&b->refs);
                    free(rc);
                    h->resolved = true; // give up on it
                }
            }
            LeaveCriticalSection(&b->cs);
            MP_THREAD_GIL_EXIT();
            Sleep(20);
            MP_THREAD_GIL_ENTER();
            mp_handle_pending(true);
        }
        nlr_pop();
    } else {
        raised = true;
        exc = nlr.ret_val;
    }

    DnsServiceBrowseCancel(&b->cancel);
    size_t n = 0;
    EnterCriticalSection(&b->cs);
    for (size_t i = 0; i < b->n; i++) {
        hit_t *h = &b->hits[i];
        if (h->resolving) {
            DnsServiceResolveCancel(&h->cancel);
        }
        if (raised || n == out_len) {
            continue;
        }
        mdns_found_t *f = &out[n++];
        memset(f, 0, sizeof(*f));
        char label[256], suffix[210];
        mdns_copy_str(label, h->full, sizeof(label));
        snprintf(suffix, sizeof(suffix), ".%s", svc);
        strip_suffix(label, suffix);
        mdns_copy_str(f->instance_name, label, sizeof(f->instance_name));
        char host[256];
        mdns_copy_str(host, h->host, sizeof(host));
        strip_suffix(host, ".");
        strip_suffix(host, ".local");
        mdns_copy_str(f->hostname, host, sizeof(f->hostname));
        mdns_copy_str(f->service_type, service_type, sizeof(f->service_type));
        mdns_copy_str(f->protocol, protocol, sizeof(f->protocol));
        f->port = h->port;
        f->has_ipv4 = h->has_ipv4;
        memcpy(f->ipv4, h->ipv4, 4);
    }
    LeaveCriticalSection(&b->cs);
    browse_release(b);
    if (raised) {
        nlr_jump(exc);
    }
    return n;
}

#endif // MDNS_PORT_WINDOWS
