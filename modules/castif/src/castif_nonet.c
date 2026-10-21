// castif on a firmware with no network stack: CircuitPython's ESP32-P4 port,
// which doesn't drive the boards' ESP32-C6 radio yet and so leaves lwIP out.
//
// castif's start() and Hls.start() refuse first (CASTIF_NO_NETWORK), so none
// of these runs; they are here so castif links. Each is weak, so a firmware
// that does carry lwIP links lwIP's own.

#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/def.h"

#define CASTIF_NONET __attribute__((weak))

static int castif_nonet(void) {
    errno = ENOSYS;
    return -1;
}

CASTIF_NONET int lwip_socket(int domain, int type, int protocol) {
    (void)domain; (void)type; (void)protocol;
    return castif_nonet();
}

CASTIF_NONET int lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
    return castif_nonet();
}

CASTIF_NONET int lwip_bind(int s, const struct sockaddr *name, socklen_t namelen) {
    (void)s; (void)name; (void)namelen;
    return castif_nonet();
}

CASTIF_NONET int lwip_listen(int s, int backlog) {
    (void)s; (void)backlog;
    return castif_nonet();
}

CASTIF_NONET int lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    (void)s; (void)addr; (void)addrlen;
    return castif_nonet();
}

CASTIF_NONET int lwip_close(int s) {
    (void)s;
    return castif_nonet();
}

CASTIF_NONET int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset, struct timeval *timeout) {
    (void)maxfdp1; (void)readset; (void)writeset; (void)exceptset; (void)timeout;
    return castif_nonet();
}

CASTIF_NONET ssize_t lwip_recv(int s, void *mem, size_t len, int flags) {
    (void)s; (void)mem; (void)len; (void)flags;
    return castif_nonet();
}

CASTIF_NONET ssize_t lwip_send(int s, const void *dataptr, size_t size, int flags) {
    (void)s; (void)dataptr; (void)size; (void)flags;
    return castif_nonet();
}

CASTIF_NONET ssize_t lwip_sendto(int s, const void *dataptr, size_t size, int flags,
    const struct sockaddr *to, socklen_t tolen) {
    (void)s; (void)dataptr; (void)size; (void)flags; (void)to; (void)tolen;
    return castif_nonet();
}

// A little-endian chip: network order is the bytes swapped.
CASTIF_NONET u16_t lwip_htons(u16_t n) {
    return (u16_t)((n << 8) | (n >> 8));
}

CASTIF_NONET u32_t lwip_htonl(u32_t n) {
    return __builtin_bswap32(n);
}

// inet_addr(): IPADDR_NONE, an address that isn't one.
CASTIF_NONET u32_t ipaddr_addr(const char *cp) {
    (void)cp;
    return IPADDR_NONE;
}
