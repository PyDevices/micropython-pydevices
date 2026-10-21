// mdns on unix: a responder and a querier of our own, on plain sockets.
//
// The responder is a thread bound to 224.0.0.251:5353 beside whatever else
// holds it (Avahi, systemd-resolved: SO_REUSEADDR and SO_REUSEPORT). It
// answers for <hostname>.local and for each advertised service: the PTR that
// lists it, its SRV and TXT, the DNS-SD service enumeration (RFC 6763 9), and
// legacy unicast questions (RFC 6762 6.7) straight back to the asker. It
// announces a service twice when it appears or changes and says goodbye (TTL
// 0) when it goes. It doesn't probe for name conflicts.
//
// find() asks from 5353 when the responder holds it, and the responder's
// thread hands it every answer that arrives there. Otherwise it asks from a
// spare port, and responders answer such a question straight back to it
// (RFC 6762 6.7). The first way also works where only multicast gets in:
// WSL's mirrored networking delivers the group's traffic but not a
// responder's unicast answer to a spare port. It asks again halfway through
// (multicast gets lost on Wi-Fi) and then asks for any port or address the
// first answers left out.
//
// SPDX-License-Identifier: MIT

#include "mdns_port.h"

#if MDNS_PORT_POSIX

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "py/runtime.h"

#define MDNS_GROUP "224.0.0.251"
#define MDNS_PORT (5353)
#define PKT_MAX (9000)
#define TXT_MAX (1300)

#define T_A (1)
#define T_PTR (12)
#define T_TXT (16)
#define T_SRV (33)
#define T_ANY (255)
#define CLASS_IN (1)
#define CACHE_FLUSH (0x8000)

#define TTL_HOST (120)
#define TTL_OTHER (4500)

// --- the responder's state, shared with its thread under lock ------------------

typedef struct recs recs_t;
static void take_records(recs_t *r, const uint8_t *msg, size_t len);

typedef struct {
    bool used;
    char type[MDNS_NAME_MAX];
    char proto[MDNS_PROTO_MAX];
    uint16_t port;
    uint8_t txt[TXT_MAX]; // RDATA: length-prefixed strings
    size_t txt_len;
} svc_t;

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_live;
    volatile bool running;
    int fd; // bound to 5353, or -1
    int wake[2];
    uint8_t ip[4];
    char host[MDNS_NAME_MAX];
    char instance[MDNS_NAME_MAX];
    svc_t svcs[MDNS_MAX_SERVICES];
    int announce_left; // announcements still to send, a second apart
    recs_t *collect; // a find() in progress gathers answers here
} g = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .wake = {-1, -1} };

// --- writing DNS messages ------------------------------------------------------

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    bool overflow;
} wr_t;

static void put(wr_t *w, const void *p, size_t n) {
    if (w->len + n > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void put16(wr_t *w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 2);
}

static void put32(wr_t *w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    put(w, b, 4);
}

static void put_label(wr_t *w, const char *s, size_t n) {
    if (n > 63) {
        n = 63;
    }
    uint8_t len = (uint8_t)n;
    put(w, &len, 1);
    put(w, s, n);
}

// A dotted name, label by label; "local" ends every name here.
static void put_dotted(wr_t *w, const char *name) {
    const char *s = name;
    while (*s) {
        const char *dot = strchr(s, '.');
        size_t n = dot ? (size_t)(dot - s) : strlen(s);
        if (n) {
            put_label(w, s, n);
        }
        if (!dot) {
            break;
        }
        s = dot + 1;
    }
}

static void put_end(wr_t *w) {
    uint8_t z = 0;
    put(w, &z, 1);
}

// Each kind of name we answer for.
static void name_services(wr_t *w) {
    put_dotted(w, "_services._dns-sd._udp.local");
    put_end(w);
}

static void name_type(wr_t *w, const svc_t *s) {
    put_label(w, s->type, strlen(s->type));
    put_label(w, s->proto, strlen(s->proto));
    put_dotted(w, "local");
    put_end(w);
}

static void name_instance(wr_t *w, const svc_t *s) {
    // The instance name is one label, dots and spaces and all.
    put_label(w, g.instance, strlen(g.instance));
    name_type(w, s);
}

static void name_host(wr_t *w) {
    put_label(w, g.host, strlen(g.host));
    put_dotted(w, "local");
    put_end(w);
}

typedef enum { R_ENUM, R_PTR, R_SRV, R_TXT, R_A } rkind_t;

// One resource record; RDLENGTH is filled in after the RDATA.
static void put_rr(wr_t *w, rkind_t kind, const svc_t *s, uint32_t ttl, bool flush) {
    uint16_t type;
    switch (kind) {
        case R_ENUM:
            name_services(w);
            type = T_PTR;
            break;
        case R_PTR:
            name_type(w, s);
            type = T_PTR;
            break;
        case R_SRV:
            name_instance(w, s);
            type = T_SRV;
            break;
        case R_TXT:
            name_instance(w, s);
            type = T_TXT;
            break;
        default:
            name_host(w);
            type = T_A;
            break;
    }
    put16(w, type);
    // Shared records (PTR) never set cache-flush; unique ones may.
    put16(w, CLASS_IN | ((flush && type != T_PTR) ? CACHE_FLUSH : 0));
    put32(w, ttl);
    size_t at = w->len;
    put16(w, 0);
    size_t start = w->len;
    switch (kind) {
        case R_ENUM:
            name_type(w, s);
            break;
        case R_PTR:
            name_instance(w, s);
            break;
        case R_SRV:
            put16(w, 0); // priority
            put16(w, 0); // weight
            put16(w, s->port);
            name_host(w);
            break;
        case R_TXT:
            if (s->txt_len) {
                put(w, s->txt, s->txt_len);
            } else {
                put_end(w); // one empty string: RFC 6763 6.1
            }
            break;
        default:
            put(w, g.ip, 4);
            break;
    }
    if (!w->overflow) {
        size_t n = w->len - start;
        w->buf[at] = (uint8_t)(n >> 8);
        w->buf[at + 1] = (uint8_t)n;
    }
}

// --- reading DNS messages ------------------------------------------------------

// The name at *off as a dotted string (labels joined with '.', following
// compression pointers); *off moves past it. False if it's malformed.
static bool read_name(const uint8_t *msg, size_t len, size_t *off, char *out, size_t out_cap) {
    size_t o = *off, n = 0;
    bool jumped = false;
    int hops = 0;
    for (;;) {
        if (o >= len) {
            return false;
        }
        uint8_t l = msg[o];
        if (l == 0) {
            if (!jumped) {
                *off = o + 1;
            }
            break;
        }
        if ((l & 0xc0) == 0xc0) {
            if (o + 1 >= len || ++hops > 32) {
                return false;
            }
            if (!jumped) {
                *off = o + 2;
                jumped = true;
            }
            o = ((size_t)(l & 0x3f) << 8) | msg[o + 1];
            continue;
        }
        if ((l & 0xc0) != 0 || o + 1 + l > len) {
            return false;
        }
        if (n + l + 2 > out_cap) {
            return false;
        }
        if (n) {
            out[n++] = '.';
        }
        memcpy(out + n, msg + o + 1, l);
        n += l;
        o += 1 + l;
    }
    out[n] = '\0';
    return true;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

// "<a>.<b>.local", compared without case.
static bool is_name(const char *got, const char *a, const char *b) {
    char want[3 * MDNS_NAME_MAX + 16];
    if (b) {
        snprintf(want, sizeof(want), "%s.%s.local", a, b);
    } else {
        snprintf(want, sizeof(want), "%s.local", a);
    }
    return strcasecmp(got, want) == 0;
}

static bool is_type_name(const char *got, const svc_t *s) {
    return is_name(got, s->type, s->proto);
}

static bool is_instance_name(const char *got, const svc_t *s) {
    char want[3 * MDNS_NAME_MAX + 16];
    snprintf(want, sizeof(want), "%s.%s.%s.local", g.instance, s->type, s->proto);
    return strcasecmp(got, want) == 0;
}

// --- the responder -------------------------------------------------------------

static void send_to(const uint8_t *pkt, size_t n, const struct sockaddr_in *dst) {
    if (g.fd >= 0 && n > 12) {
        sendto(g.fd, pkt, n, 0, (const struct sockaddr *)dst, sizeof(*dst));
    }
}

static void group_addr(struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family = AF_INET;
    a->sin_port = htons(MDNS_PORT);
    a->sin_addr.s_addr = inet_addr(MDNS_GROUP);
}

// Every record of one service (or all of them, which < 0), plus our address.
// Called with the lock held.
static size_t build_announce(uint8_t *pkt, size_t cap, int which, uint32_t ttl) {
    wr_t w = { pkt, 12, cap, false };
    memset(pkt, 0, 12);
    pkt[2] = 0x84;
    uint16_t count = 0;
    for (int i = 0; i < MDNS_MAX_SERVICES; i++) {
        const svc_t *s = &g.svcs[i];
        if (!s->used || (which >= 0 && which != i)) {
            continue;
        }
        put_rr(&w, R_ENUM, s, ttl ? TTL_OTHER : 0, true);
        put_rr(&w, R_PTR, s, ttl ? TTL_OTHER : 0, true);
        put_rr(&w, R_SRV, s, ttl ? TTL_HOST : 0, true);
        put_rr(&w, R_TXT, s, ttl ? TTL_OTHER : 0, true);
        count += 4;
    }
    if (ttl) {
        put_rr(&w, R_A, NULL, TTL_HOST, true);
        count++;
    }
    if (w.overflow || count == 0) {
        return 0;
    }
    pkt[6] = (uint8_t)(count >> 8);
    pkt[7] = (uint8_t)count;
    return w.len;
}

static void announce_locked(void) {
    static uint8_t pkt[PKT_MAX];
    size_t n = build_announce(pkt, sizeof(pkt), -1, 1);
    struct sockaddr_in dst;
    group_addr(&dst);
    send_to(pkt, n, &dst);
}

static void goodbye_locked(int which) {
    static uint8_t pkt[PKT_MAX];
    size_t n = build_announce(pkt, sizeof(pkt), which, 0);
    struct sockaddr_in dst;
    group_addr(&dst);
    send_to(pkt, n, &dst);
}

typedef struct {
    rkind_t kind;
    int svc;
} want_t;

#define WANT_MAX (4 * MDNS_MAX_SERVICES + 2)

static void add_want(want_t *wants, int *n, rkind_t kind, int svc) {
    for (int i = 0; i < *n; i++) {
        if (wants[i].kind == kind && wants[i].svc == svc) {
            return;
        }
    }
    if (*n < WANT_MAX) {
        wants[*n].kind = kind;
        wants[*n].svc = svc;
        (*n)++;
    }
}

static bool type_matches(uint16_t qtype, uint16_t t) {
    return qtype == t || qtype == T_ANY;
}

// Answer one query; called with the lock held.
static void handle_query(const uint8_t *msg, size_t len, const struct sockaddr_in *src) {
    if (len < 12 || (msg[2] & 0x80)) {
        return; // a response, or nothing
    }
    // A legacy unicast query comes from a port other than 5353; its answer
    // goes straight back, echoing the ID and question, with short TTLs.
    bool legacy = ntohs(src->sin_port) != MDNS_PORT;
    uint16_t qd = get16(msg + 4);
    want_t answers[WANT_MAX], extras[WANT_MAX];
    int na = 0, ne = 0;
    size_t off = 12;
    size_t q_start = off;
    char name[256];
    for (uint16_t i = 0; i < qd; i++) {
        if (!read_name(msg, len, &off, name, sizeof(name)) || off + 4 > len) {
            return;
        }
        uint16_t qtype = get16(msg + off);
        off += 4;
        if (strcasecmp(name, "_services._dns-sd._udp.local") == 0 && type_matches(qtype, T_PTR)) {
            for (int k = 0; k < MDNS_MAX_SERVICES; k++) {
                if (g.svcs[k].used) {
                    add_want(answers, &na, R_ENUM, k);
                }
            }
        }
        if (is_name(name, g.host, NULL) && type_matches(qtype, T_A)) {
            add_want(answers, &na, R_A, -1);
        }
        for (int k = 0; k < MDNS_MAX_SERVICES; k++) {
            const svc_t *s = &g.svcs[k];
            if (!s->used) {
                continue;
            }
            if (is_type_name(name, s) && type_matches(qtype, T_PTR)) {
                add_want(answers, &na, R_PTR, k);
                add_want(extras, &ne, R_SRV, k);
                add_want(extras, &ne, R_TXT, k);
                add_want(extras, &ne, R_A, -1);
            }
            if (is_instance_name(name, s)) {
                if (type_matches(qtype, T_SRV)) {
                    add_want(answers, &na, R_SRV, k);
                    add_want(extras, &ne, R_A, -1);
                }
                if (type_matches(qtype, T_TXT)) {
                    add_want(answers, &na, R_TXT, k);
                }
            }
        }
    }
    if (na == 0) {
        return;
    }
    static uint8_t pkt[PKT_MAX];
    wr_t w = { pkt, 12, sizeof(pkt), false };
    memset(pkt, 0, 12);
    pkt[2] = 0x84;
    if (legacy) {
        pkt[0] = msg[0];
        pkt[1] = msg[1];
        put(&w, msg + q_start, off - q_start);
        pkt[5] = (uint8_t)qd;
        pkt[4] = (uint8_t)(qd >> 8);
    }
    for (int i = 0; i < na; i++) {
        const svc_t *s = answers[i].svc >= 0 ? &g.svcs[answers[i].svc] : NULL;
        uint32_t ttl = legacy ? 10 : (answers[i].kind == R_SRV || answers[i].kind == R_A ? TTL_HOST : TTL_OTHER);
        put_rr(&w, answers[i].kind, s, ttl, !legacy);
    }
    int nx = 0;
    for (int i = 0; i < ne; i++) {
        bool dup = false;
        for (int j = 0; j < na; j++) {
            dup |= answers[j].kind == extras[i].kind && answers[j].svc == extras[i].svc;
        }
        if (dup) {
            continue;
        }
        const svc_t *s = extras[i].svc >= 0 ? &g.svcs[extras[i].svc] : NULL;
        uint32_t ttl = legacy ? 10 : (extras[i].kind == R_SRV || extras[i].kind == R_A ? TTL_HOST : TTL_OTHER);
        put_rr(&w, extras[i].kind, s, ttl, !legacy);
        nx++;
    }
    if (w.overflow) {
        return;
    }
    pkt[6] = (uint8_t)(na >> 8);
    pkt[7] = (uint8_t)na;
    pkt[10] = (uint8_t)(nx >> 8);
    pkt[11] = (uint8_t)nx;
    if (legacy) {
        send_to(pkt, w.len, src);
    } else {
        struct sockaddr_in dst;
        group_addr(&dst);
        send_to(pkt, w.len, &dst);
    }
}

static void *responder_thread(void *arg) {
    (void)arg;
    static uint8_t msg[PKT_MAX];
    while (g.running) {
        struct pollfd fds[2] = {
            { .fd = g.fd, .events = POLLIN },
            { .fd = g.wake[0], .events = POLLIN },
        };
        pthread_mutex_lock(&g.lock);
        int wait_ms = g.announce_left > 0 ? 1000 : -1;
        pthread_mutex_unlock(&g.lock);
        int rc = poll(fds, 2, wait_ms);
        if (!g.running) {
            break;
        }
        if (rc == 0) {
            pthread_mutex_lock(&g.lock);
            if (g.announce_left > 0) {
                g.announce_left--;
                announce_locked();
            }
            pthread_mutex_unlock(&g.lock);
            continue;
        }
        if (rc < 0) {
            continue;
        }
        if (fds[1].revents & POLLIN) {
            char b[16];
            // Only the wake-up matters, not the bytes; the result is kept so
            // glibc's warn_unused_result stays quiet (CircuitPython makes it an error).
            ssize_t drained = read(g.wake[0], b, sizeof(b));
            (void)drained;
        }
        if (fds[0].revents & POLLIN) {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            ssize_t n = recvfrom(g.fd, msg, sizeof(msg), 0, (struct sockaddr *)&src, &slen);
            if (n > 0) {
                pthread_mutex_lock(&g.lock);
                if (msg[2] & 0x80) {
                    if (g.collect) {
                        take_records(g.collect, msg, (size_t)n);
                    }
                } else {
                    handle_query(msg, (size_t)n, &src);
                }
                pthread_mutex_unlock(&g.lock);
            }
        }
    }
    return NULL;
}

static void wake_thread(void) {
    if (g.wake[1] >= 0) {
        // A full pipe already has a wake-up in it, so the result is not needed.
        ssize_t sent = write(g.wake[1], "x", 1);
        (void)sent;
    }
}

// Bind 5353 and start the thread; false if 5353 can't be had.
static bool responder_start(void) {
    if (g.thread_live) {
        return true;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    #ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    #endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(MDNS_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        close(fd);
        return false;
    }
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    memcpy(&mreq.imr_interface.s_addr, g.ip, 4);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
            close(fd);
            return false;
        }
    }
    unsigned char ttl = 255, loop = 1;
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    struct in_addr iface;
    memcpy(&iface.s_addr, g.ip, 4);
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
    if (pipe(g.wake) != 0) {
        close(fd);
        return false;
    }
    g.fd = fd;
    g.running = true;
    if (pthread_create(&g.thread, NULL, responder_thread, NULL) != 0) {
        g.running = false;
        close(fd);
        close(g.wake[0]);
        close(g.wake[1]);
        g.fd = g.wake[0] = g.wake[1] = -1;
        return false;
    }
    g.thread_live = true;
    return true;
}

static void responder_stop(void) {
    if (!g.thread_live) {
        return;
    }
    g.running = false;
    wake_thread();
    pthread_join(g.thread, NULL);
    g.thread_live = false;
    close(g.fd);
    close(g.wake[0]);
    close(g.wake[1]);
    g.fd = g.wake[0] = g.wake[1] = -1;
}

// The IPv4 address multicast leaves from: the one the route to the mDNS
// group uses, or failing that the default route's.
static bool local_ipv4(uint8_t ip[4]) {
    static const char *const via[] = { MDNS_GROUP, "8.8.8.8" };
    for (size_t i = 0; i < 2; i++) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            return false;
        }
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(MDNS_PORT);
        a.sin_addr.s_addr = inet_addr(via[i]);
        socklen_t alen = sizeof(a);
        bool ok = connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0
            && getsockname(fd, (struct sockaddr *)&a, &alen) == 0
            && a.sin_addr.s_addr != htonl(INADDR_ANY);
        close(fd);
        if (ok) {
            memcpy(ip, &a.sin_addr.s_addr, 4);
            return true;
        }
    }
    return false;
}

// --- the port API --------------------------------------------------------------

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
    if (network_interface == mp_const_none) {
        if (!local_ipv4(ip)) {
            mp_raise_OSError(MP_EHOSTUNREACH);
        }
    } else if (mp_obj_is_str(network_interface)) {
        // A computer on more than one network: the address to work from.
        struct in_addr a;
        if (inet_pton(AF_INET, mp_obj_str_get_str(network_interface), &a) != 1) {
            mp_raise_ValueError(MP_ERROR_TEXT("network_interface must be None or an IPv4 address"));
        }
        memcpy(ip, &a.s_addr, 4);
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("network_interface must be None or an IPv4 address"));
    }
    char host[256];
    if (gethostname(host, sizeof(host)) != 0 || host[0] == '\0') {
        strcpy(host, "micropython");
    }
    char *dot = strchr(host, '.');
    if (dot) {
        *dot = '\0';
    }
    pthread_mutex_lock(&g.lock);
    memcpy(g.ip, ip, 4);
    mdns_copy_str(g.host, host, sizeof(g.host));
    mdns_copy_str(g.instance, host, sizeof(g.instance));
    memset(g.svcs, 0, sizeof(g.svcs));
    g.announce_left = 0;
    pthread_mutex_unlock(&g.lock);
    mdns_copy_str(self->hostname, host, sizeof(self->hostname));
    // Answering for <hostname>.local needs 5353; finding things doesn't, so
    // a host where it can't be had still finds, and advertise_service() says.
    responder_start();
}

void mdns_port_deinit(mdns_server_obj_t *self) {
    (void)self;
    pthread_mutex_lock(&g.lock);
    goodbye_locked(-1);
    memset(g.svcs, 0, sizeof(g.svcs));
    g.announce_left = 0;
    pthread_mutex_unlock(&g.lock);
    responder_stop();
}

void mdns_port_set_hostname(mdns_server_obj_t *self, const char *hostname) {
    (void)self;
    pthread_mutex_lock(&g.lock);
    mdns_copy_str(g.host, hostname, sizeof(g.host));
    g.announce_left = 2;
    announce_locked();
    pthread_mutex_unlock(&g.lock);
    wake_thread();
}

void mdns_port_set_instance_name(mdns_server_obj_t *self, const char *instance_name) {
    (void)self;
    pthread_mutex_lock(&g.lock);
    goodbye_locked(-1); // the old instance names
    mdns_copy_str(g.instance, instance_name, sizeof(g.instance));
    g.announce_left = 2;
    announce_locked();
    pthread_mutex_unlock(&g.lock);
    wake_thread();
}

void mdns_port_advertise(mdns_server_obj_t *self, bool update, const char *service_type, const char *protocol,
    uint16_t port, const char **txt, const size_t *txt_len, size_t num_txt) {
    (void)self;
    (void)update;
    size_t total = 0;
    for (size_t i = 0; i < num_txt; i++) {
        total += 1 + txt_len[i];
    }
    if (total > TXT_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("Failed to add service TXT record"));
    }
    if (!responder_start()) {
        mp_raise_OSError(MP_EADDRINUSE);
    }
    pthread_mutex_lock(&g.lock);
    svc_t *s = NULL;
    for (int i = 0; i < MDNS_MAX_SERVICES && !s; i++) {
        if (g.svcs[i].used && strcmp(g.svcs[i].type, service_type) == 0 && strcmp(g.svcs[i].proto, protocol) == 0) {
            s = &g.svcs[i];
        }
    }
    for (int i = 0; i < MDNS_MAX_SERVICES && !s; i++) {
        if (!g.svcs[i].used) {
            s = &g.svcs[i];
        }
    }
    if (s == NULL) {
        pthread_mutex_unlock(&g.lock);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Out of MDNS service slots"));
    }
    s->used = true;
    mdns_copy_str(s->type, service_type, sizeof(s->type));
    mdns_copy_str(s->proto, protocol, sizeof(s->proto));
    s->port = port;
    s->txt_len = 0;
    for (size_t i = 0; i < num_txt; i++) {
        s->txt[s->txt_len++] = (uint8_t)txt_len[i];
        memcpy(s->txt + s->txt_len, txt[i], txt_len[i]);
        s->txt_len += txt_len[i];
    }
    g.announce_left = 2;
    announce_locked();
    pthread_mutex_unlock(&g.lock);
    wake_thread();
}

// --- find(): the querier ---------------------------------------------------------

typedef struct {
    char name[256];
    uint16_t type;
    uint16_t port; // SRV
    char target[256]; // PTR, SRV
    uint8_t a[4]; // A
} rec_t;

struct recs {
    rec_t *recs;
    size_t n, cap;
};

static const rec_t *find_rec(const recs_t *r, const char *name, uint16_t type) {
    for (size_t i = 0; i < r->n; i++) {
        if (r->recs[i].type == type && strcasecmp(r->recs[i].name, name) == 0) {
            return &r->recs[i];
        }
    }
    return NULL;
}

static void keep_rec(recs_t *r, const rec_t *rec) {
    for (size_t i = 0; i < r->n; i++) {
        const rec_t *o = &r->recs[i];
        if (o->type == rec->type && strcasecmp(o->name, rec->name) == 0
            && strcasecmp(o->target, rec->target) == 0 && o->port == rec->port
            && memcmp(o->a, rec->a, 4) == 0) {
            return;
        }
    }
    if (r->n == 512) {
        return; // enough: the rest of a busy network's chatter can go
    }
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 32;
        rec_t *grown = realloc(r->recs, cap * sizeof(rec_t));
        if (grown == NULL) {
            return;
        }
        r->recs = grown;
        r->cap = cap;
    }
    r->recs[r->n++] = *rec;
}

// Every A, PTR and SRV record in a message (answers, authority, additional).
static void take_records(recs_t *r, const uint8_t *msg, size_t len) {
    if (len < 12 || !(msg[2] & 0x80)) {
        return;
    }
    size_t off = 12;
    char skip[256];
    for (uint16_t i = 0, qd = get16(msg + 4); i < qd; i++) {
        if (!read_name(msg, len, &off, skip, sizeof(skip)) || off + 4 > len) {
            return;
        }
        off += 4;
    }
    unsigned count = get16(msg + 6) + get16(msg + 8) + get16(msg + 10);
    for (unsigned i = 0; i < count; i++) {
        rec_t rec;
        memset(&rec, 0, sizeof(rec));
        if (!read_name(msg, len, &off, rec.name, sizeof(rec.name)) || off + 10 > len) {
            return;
        }
        rec.type = get16(msg + off);
        uint16_t rdlen = get16(msg + off + 8);
        off += 10;
        if (off + rdlen > len) {
            return;
        }
        size_t at = off;
        bool keep = false;
        if (rec.type == T_A && rdlen == 4) {
            memcpy(rec.a, msg + at, 4);
            keep = true;
        } else if (rec.type == T_PTR) {
            keep = read_name(msg, len, &at, rec.target, sizeof(rec.target));
        } else if (rec.type == T_SRV && rdlen >= 7) {
            rec.port = get16(msg + at + 4);
            at += 6;
            keep = read_name(msg, len, &at, rec.target, sizeof(rec.target));
        }
        if (keep) {
            keep_rec(r, &rec);
        }
        off += rdlen;
    }
}

static mp_uint_t elapsed_ms(mp_uint_t start) {
    return mp_hal_ticks_ms() - start;
}

typedef struct {
    int fd; // the spare port's socket, or -1 to ask from 5353
    recs_t recs;
} ask_t;

static size_t build_query(uint8_t *pkt, size_t cap, uint16_t ident, const char (*names)[256], const uint16_t *types, size_t n) {
    wr_t w = { pkt, 12, cap, false };
    memset(pkt, 0, 12);
    pkt[0] = (uint8_t)(ident >> 8);
    pkt[1] = (uint8_t)ident;
    for (size_t i = 0; i < n; i++) {
        put_dotted(&w, names[i]);
        put_end(&w);
        put16(&w, types[i]);
        put16(&w, CLASS_IN);
    }
    pkt[4] = (uint8_t)(n >> 8);
    pkt[5] = (uint8_t)n;
    return w.overflow ? 0 : w.len;
}

static void ask_send(ask_t *q, const uint8_t *pkt, size_t len, const struct sockaddr_in *dst) {
    if (len == 0) {
        return;
    }
    if (q->fd >= 0) {
        sendto(q->fd, pkt, len, 0, (const struct sockaddr *)dst, sizeof(*dst));
    } else {
        pthread_mutex_lock(&g.lock);
        send_to(pkt, len, dst);
        pthread_mutex_unlock(&g.lock);
    }
}

// The thread stops adding to q's records, so they can be read without the lock.
static void ask_stop(ask_t *q) {
    if (q->fd < 0) {
        pthread_mutex_lock(&g.lock);
        g.collect = NULL;
        pthread_mutex_unlock(&g.lock);
    }
}

// Send the questions, then gather answers for ms milliseconds, asking again
// halfway. Ctrl-C raises out of here; the caller closes the socket.
static void ask(ask_t *q, const char (*names)[256], const uint16_t *types, size_t n, uint16_t ident, mp_int_t ms) {
    static uint8_t pkt[PKT_MAX];
    size_t len = build_query(pkt, sizeof(pkt), ident, names, types, n);
    struct sockaddr_in dst;
    group_addr(&dst);
    if (q->fd < 0) {
        pthread_mutex_lock(&g.lock);
        g.collect = &q->recs;
        pthread_mutex_unlock(&g.lock);
    }
    ask_send(q, pkt, len, &dst);
    mp_uint_t start = mp_hal_ticks_ms();
    bool resent = false;
    uint8_t *msg = malloc(PKT_MAX);
    if (msg == NULL) {
        return;
    }
    for (;;) {
        mp_int_t left = ms - (mp_int_t)elapsed_ms(start);
        if (left <= 0) {
            break;
        }
        if (!resent && left < ms / 2) {
            ask_send(q, pkt, len, &dst);
            resent = true;
        }
        struct pollfd pfd = { .fd = q->fd, .events = POLLIN };
        int rc;
        MP_THREAD_GIL_EXIT();
        rc = poll(&pfd, q->fd < 0 ? 0 : 1, left < 50 ? (int)left : 50);
        MP_THREAD_GIL_ENTER();
        if (rc > 0) {
            ssize_t got = recv(q->fd, msg, PKT_MAX, 0);
            if (got > 0) {
                take_records(&q->recs, msg, (size_t)got);
            }
        }
        // Ctrl-C ends the search; the caller cleans up.
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_handle_pending(true);
            nlr_pop();
        } else {
            ask_stop(q);
            free(msg);
            nlr_jump(nlr.ret_val);
        }
    }
    ask_stop(q);
    free(msg);
}

static void strip_local(char *name) {
    size_t n = strlen(name);
    if (n > 6 && strcasecmp(name + n - 6, ".local") == 0) {
        name[n - 6] = '\0';
    }
}

size_t mdns_port_find(mdns_server_obj_t *self, const char *service_type, const char *protocol,
    mp_int_t timeout_ms, mdns_found_t *out, size_t out_len) {
    (void)self;
    ask_t q = { .fd = -1 };
    pthread_mutex_lock(&g.lock);
    bool from_5353 = g.thread_live;
    pthread_mutex_unlock(&g.lock);
    if (!from_5353) {
        q.fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (q.fd < 0) {
            mp_raise_OSError(errno);
        }
    }
    uint8_t ip[4];
    pthread_mutex_lock(&g.lock);
    memcpy(ip, g.ip, 4);
    pthread_mutex_unlock(&g.lock);
    if (q.fd >= 0) {
        struct in_addr iface;
        memcpy(&iface.s_addr, ip, 4);
        setsockopt(q.fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
        unsigned char ttl = 255, loop = 1;
        setsockopt(q.fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        setsockopt(q.fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    }

    char (*names)[256] = calloc(2 * MDNS_FIND_MAX + 1, 256);
    uint16_t *types = calloc(2 * MDNS_FIND_MAX + 1, sizeof(uint16_t));
    char (*inst)[256] = calloc(MDNS_FIND_MAX, 256);
    size_t n = 0;
    nlr_buf_t nlr;
    if (names == NULL || types == NULL || inst == NULL) {
        goto done;
    }
    if (nlr_push(&nlr) == 0) {
        char svc[256];
        snprintf(svc, sizeof(svc), "%s.%s.local", service_type, protocol);
        // A query from 5353 is an ordinary mDNS one, whose ID is zero (RFC 6762 18.1).
        uint16_t ident = q.fd < 0 ? 0 : (uint16_t)(mp_hal_ticks_ms() | 1);
        mdns_copy_str(names[0], svc, 256);
        types[0] = T_PTR;
        ask(&q, (const char (*)[256])names, types, 1, ident, timeout_ms * 6 / 10);

        // The instances, once each, as first spelled.
        size_t ni = 0;
        for (size_t i = 0; i < q.recs.n && ni < out_len && ni < MDNS_FIND_MAX; i++) {
            const rec_t *r = &q.recs.recs[i];
            if (r->type != T_PTR || strcasecmp(r->name, svc) != 0) {
                continue;
            }
            bool seen = false;
            for (size_t j = 0; j < ni; j++) {
                seen |= strcasecmp(inst[j], r->target) == 0;
            }
            if (!seen) {
                mdns_copy_str(inst[ni++], r->target, 256);
            }
        }
        // Some responders send the port and address with the PTR; for the
        // rest, ask for what's missing.
        size_t nf = 0;
        for (size_t i = 0; i < ni; i++) {
            const rec_t *srv = find_rec(&q.recs, inst[i], T_SRV);
            if (srv == NULL) {
                mdns_copy_str(names[nf], inst[i], 256);
                types[nf++] = T_SRV;
            } else if (find_rec(&q.recs, srv->target, T_A) == NULL) {
                mdns_copy_str(names[nf], srv->target, 256);
                types[nf++] = T_A;
            }
        }
        if (nf) {
            ask(&q, (const char (*)[256])names, types, nf, ident ? ident + 1 : 0, timeout_ms * 4 / 10);
        }
        for (size_t i = 0; i < ni; i++) {
            mdns_found_t *f = &out[n++];
            memset(f, 0, sizeof(*f));
            // The instance name is what comes before ".<service>.local".
            size_t il = strlen(inst[i]), sl = strlen(svc);
            char label[256];
            mdns_copy_str(label, inst[i], sizeof(label));
            if (il > sl + 1 && strcasecmp(inst[i] + il - sl, svc) == 0) {
                label[il - sl - 1] = '\0';
            }
            mdns_copy_str(f->instance_name, label, sizeof(f->instance_name));
            mdns_copy_str(f->service_type, service_type, sizeof(f->service_type));
            mdns_copy_str(f->protocol, protocol, sizeof(f->protocol));
            const rec_t *srv = find_rec(&q.recs, inst[i], T_SRV);
            if (srv) {
                f->port = srv->port;
                char host[256];
                mdns_copy_str(host, srv->target, sizeof(host));
                const rec_t *a = find_rec(&q.recs, host, T_A);
                if (a) {
                    memcpy(f->ipv4, a->a, 4);
                    f->has_ipv4 = true;
                }
                strip_local(host);
                mdns_copy_str(f->hostname, host, sizeof(f->hostname));
            }
        }
        nlr_pop();
    } else {
        if (q.fd >= 0) {
            close(q.fd);
        }
        free(q.recs.recs);
        free(names);
        free(types);
        free(inst);
        nlr_jump(nlr.ret_val);
    }
done:
    if (q.fd >= 0) {
            close(q.fd);
        }
    free(q.recs.recs);
    free(names);
    free(types);
    free(inst);
    return n;
}

#endif // MDNS_PORT_POSIX
