// SPDX-License-Identifier: MIT
// What castif_hls.c needs from MicroPython, FreeRTOS, ESP-IDF and lwIP to
// compile on a PC for tests/test_castif_hls.c: one struct, one handle type,
// realloc for PSRAM, and the BSD sockets lwIP mirrors.
#ifndef CASTIF_HOST_RUNTIME_H
#define CASTIF_HOST_RUNTIME_H

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { const void *type; } mp_obj_base_t;
typedef void *TaskHandle_t;

#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_realloc(void *p, size_t n, int caps) {
    (void)caps;
    return realloc(p, n);
}
#define closesocket close

#endif
