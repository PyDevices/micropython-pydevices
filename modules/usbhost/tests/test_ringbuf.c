// SPDX-License-Identifier: MIT
//
// Host-side tests for the SPSC event ring. Runs on any C compiler with no
// MicroPython, no IDF, and no board: the wraparound and full-ring behaviour is
// the part most likely to be wrong and the part hardest to observe on-target,
// so it is pinned down here where a failure is a two-second answer.
#include "shared/usbhost_ringbuf.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static usbhost_event_t make(uint32_t id) {
    usbhost_event_t e;
    memset(&e, 0, sizeof(e));
    e.kind = USBHOST_EV_ATTACH;
    e.dev_id = id;
    return e;
}

int main(void) {
    usbhost_event_t slots[4];
    usbhost_ringbuf_t rb;
    usbhost_event_t out;

    // Empty ring
    usbhost_rb_init(&rb, slots, 4);
    CHECK(usbhost_rb_count(&rb) == 0);
    CHECK(!usbhost_rb_pop(&rb, &out));
    CHECK(usbhost_rb_take_dropped(&rb) == 0);

    // FIFO order
    for (uint32_t i = 1; i <= 3; i++) {
        usbhost_event_t e = make(i);
        CHECK(usbhost_rb_push(&rb, &e));
    }
    CHECK(usbhost_rb_count(&rb) == 3);
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK(usbhost_rb_pop(&rb, &out));
        CHECK(out.dev_id == i);
    }
    CHECK(usbhost_rb_count(&rb) == 0);

    // Usable depth is capacity - 1: the fourth push into a 4-slot ring must
    // fail, which is how full stays distinguishable from empty without a
    // shared counter that an ISR would have to update.
    usbhost_rb_init(&rb, slots, 4);
    for (uint32_t i = 0; i < 3; i++) {
        usbhost_event_t e = make(i);
        CHECK(usbhost_rb_push(&rb, &e));
    }
    usbhost_event_t overflow = make(99);
    CHECK(!usbhost_rb_push(&rb, &overflow));
    CHECK(usbhost_rb_count(&rb) == 3);

    // The drop is counted, and reported exactly once.
    CHECK(usbhost_rb_take_dropped(&rb) == 1);
    CHECK(usbhost_rb_take_dropped(&rb) == 0);

    // A dropped event must not corrupt the ones already queued.
    for (uint32_t i = 0; i < 3; i++) {
        CHECK(usbhost_rb_pop(&rb, &out));
        CHECK(out.dev_id == i);
    }

    // Wraparound: interleaved push/pop far past capacity must stay in order.
    usbhost_rb_init(&rb, slots, 4);
    for (uint32_t i = 0; i < 1000; i++) {
        usbhost_event_t e = make(i);
        CHECK(usbhost_rb_push(&rb, &e));
        CHECK(usbhost_rb_pop(&rb, &out));
        CHECK(out.dev_id == i);
    }
    CHECK(usbhost_rb_count(&rb) == 0);
    CHECK(usbhost_rb_take_dropped(&rb) == 0);

    // Payload survives the round trip intact.
    usbhost_rb_init(&rb, slots, 4);
    usbhost_event_t rich;
    memset(&rich, 0, sizeof(rich));
    rich.kind = USBHOST_EV_DETACH;
    rich.speed = 2;
    rich.vid = 0x303a;
    rich.pid = 0x1001;
    rich.classes = 0x0005;
    rich.dev_id = 0xdeadbeef;
    CHECK(usbhost_rb_push(&rb, &rich));
    CHECK(usbhost_rb_pop(&rb, &out));
    CHECK(out.kind == USBHOST_EV_DETACH && out.speed == 2);
    CHECK(out.vid == 0x303a && out.pid == 0x1001);
    CHECK(out.classes == 0x0005 && out.dev_id == 0xdeadbeef);

    // A full ring that is then drained accepts pushes again.
    usbhost_rb_init(&rb, slots, 4);
    for (uint32_t i = 0; i < 3; i++) {
        usbhost_event_t e = make(i);
        usbhost_rb_push(&rb, &e);
    }
    CHECK(!usbhost_rb_push(&rb, &overflow));
    CHECK(usbhost_rb_pop(&rb, &out));
    CHECK(usbhost_rb_push(&rb, &overflow));
    CHECK(usbhost_rb_count(&rb) == 3);

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("ringbuf: all checks passed\n");
    return 0;
}
