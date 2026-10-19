// SPDX-License-Identifier: MIT
//
// The lifetime gate behind usbhost's pcm_c_sink_t. The reasoning is in the
// header; this file is only the four atomic operations it names.
#include "shared/usbhost_pcm_sink.h"

// A test widens the race window here, inside the gate, to prove that close()
// really waits (tests/test_pcm_sink.c). Empty in firmware.
#ifndef USBHOST_PCM_SINK_INSIDE_HOOK
#define USBHOST_PCM_SINK_INSIDE_HOOK() do { } while (0)
#endif

void usbhost_pcm_sink_init(usbhost_pcm_sink_t *s, const usbhost_pcm_ring_ops_t *ops, void *ring,
    void (*wait)(void)) {
    s->ops = ops;
    s->ring = ring;
    s->frame = 0;
    s->live = 0;
    s->users = 0;
    s->busy = 0;
    s->last_gen = 0;
    s->wait = wait;
}

void usbhost_pcm_sink_set_ring(usbhost_pcm_sink_t *s, void *ring) {
    s->ring = ring;
}

uint32_t usbhost_pcm_sink_open(usbhost_pcm_sink_t *s, uint32_t frame) {
    uint32_t gen = s->last_gen + 1;
    if (gen == 0) {
        gen = 1;                // 0 means closed; four billion opens later
    }
    s->last_gen = gen;
    s->frame = frame;
    // Everything the owner set up -- the ring, `frame` -- is published by
    // this store: a caller that reads `gen` from `live` sees all of it.
    __atomic_store_n(&s->live, gen, __ATOMIC_SEQ_CST);
    return gen;
}

void usbhost_pcm_sink_close(usbhost_pcm_sink_t *s) {
    __atomic_store_n(&s->live, 0, __ATOMIC_SEQ_CST);
    #ifndef USBHOST_PCM_SINK_PLANT_NO_WAIT
    // Unbounded on purpose. A caller inside is doing one memcpy of at most
    // the ring's size; giving up and freeing anyway would trade a short
    // stall for a write into freed memory.
    while (__atomic_load_n(&s->users, __ATOMIC_SEQ_CST) != 0) {
        s->wait();
    }
    #endif
}

uint32_t usbhost_pcm_sink_current(const usbhost_pcm_sink_t *s) {
    return __atomic_load_n(&s->live, __ATOMIC_ACQUIRE);
}

static inline int usbhost_pcm_sink_enter(usbhost_pcm_sink_t *s, uint32_t gen) {
    __atomic_fetch_add(&s->users, 1, __ATOMIC_SEQ_CST);
    if (gen == 0 || __atomic_load_n(&s->live, __ATOMIC_SEQ_CST) != gen) {
        __atomic_fetch_sub(&s->users, 1, __ATOMIC_RELEASE);
        return 0;
    }
    return 1;
}

static inline void usbhost_pcm_sink_leave(usbhost_pcm_sink_t *s) {
    __atomic_fetch_sub(&s->users, 1, __ATOMIC_RELEASE);
}

int usbhost_pcm_sink_write(usbhost_pcm_sink_t *s, uint32_t gen, const uint8_t *data, size_t len) {
    if (!usbhost_pcm_sink_enter(s, gen)) {
        return -1;
    }
    USBHOST_PCM_SINK_INSIDE_HOOK();
    int taken = 0;
    #ifndef USBHOST_PCM_SINK_PLANT_NO_TRYLOCK
    if (__atomic_exchange_n(&s->busy, 1, __ATOMIC_ACQUIRE) == 0) {
    #else
    {
    #endif
        uint32_t room = s->ops->room(s->ring);
        uint32_t n = len < room ? (uint32_t)len : room;
        if (s->frame) {
            n -= n % s->frame;
        }
        if (n) {
            taken = (int)s->ops->push(s->ring, data, n);
        }
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    }
    USBHOST_PCM_SINK_INSIDE_HOOK();
    usbhost_pcm_sink_leave(s);
    return taken;
}

int usbhost_pcm_sink_space(usbhost_pcm_sink_t *s, uint32_t gen) {
    if (!usbhost_pcm_sink_enter(s, gen)) {
        return -1;
    }
    uint32_t room = s->ops->room(s->ring);
    if (s->frame) {
        room -= room % s->frame;
    }
    usbhost_pcm_sink_leave(s);
    return (int)room;
}
