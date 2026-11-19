// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The A2DP sink's ring, fed the way a phone feeds it: packets in bursts, read
// a block at a time at the output's pace. Runs on the desktop.
//
// From modules/btclassic:
//   cc -std=c11 -Wall -Wextra -Werror -Iesp-idf/btclassic -Iesp-idf/btclassic/include
//      -o /tmp/test_ring tests/test_ring.c esp-idf/btclassic/ring.c

#include <stdio.h>
#include <stdlib.h>

#include "ring.h"

#define CAPACITY 48000u
#define TARGET (CAPACITY / 2)
#define BLOCK 256u              // frames a read takes
#define PACKET 2048u            // frames a packet carries
#define BURST 2u                // packets the sender sends at a time
#define TICKS_A_SECOND (44100u / BLOCK)

static int16_t storage[CAPACITY * 2];
static int16_t packet[(PACKET + 64) * 2];
static int16_t block[BLOCK * 2];
static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
            failures++; \
        } \
} while (0)

typedef struct {
    btc_ring_t ring;
    uint32_t tick;
    uint32_t high, low;         // the fill's extremes since the last mark
} sim_t;

static void send(sim_t *s, uint32_t frames) {
    while (frames) {
        uint32_t n = frames > PACKET ? PACKET : frames;
        btc_ring_write(&s->ring, packet, n, 2);
        frames -= n;
    }
}

// SECONDS of steady streaming; each packet carries EXTRA frames beyond real time.
static void stream(sim_t *s, uint32_t seconds, uint32_t extra) {
    uint32_t every = BURST * PACKET / BLOCK;
    for (uint32_t i = 0; i < seconds * TICKS_A_SECOND; i++, s->tick++) {
        if (s->tick % every == 0) {
            send(s, BURST * (PACKET + extra));
        }
        btc_ring_read(&s->ring, block, BLOCK, 2, 16129);
        btc_ring_stats_t st;
        btc_ring_stats(&s->ring, &st);
        if (st.buffered > s->high) {
            s->high = st.buffered;
        }
        if (st.buffered < s->low) {
            s->low = st.buffered;
        }
    }
}

static void start(sim_t *s) {
    btc_ring_init(&s->ring, storage, CAPACITY, 2);
    s->tick = 0;
    send(s, TARGET);
    stream(s, 2, 0);
    s->high = 0;
    s->low = CAPACITY;
}

static btc_ring_stats_t stats(const sim_t *s) {
    btc_ring_stats_t st;
    btc_ring_stats(&s->ring, &st);
    return st;
}

// A fast start leaves the ring high once it is playing. The clocks agree, so
// nothing needs correcting and no frame may be lost to the sender's bursts.
static void test_high_after_a_fast_start_drops_nothing(void) {
    static const uint32_t extras[] = {2000, 4000, 5000, 6000};
    for (size_t i = 0; i < sizeof(extras) / sizeof(extras[0]); i++) {
        sim_t s;
        start(&s);
        send(&s, extras[i]);
        stream(&s, 60, 0);
        btc_ring_stats_t st = stats(&s);
        CHECK(st.dropped == 0, "%u frames high: dropped %u", (unsigned)extras[i], (unsigned)st.dropped);
        CHECK(st.overruns == 0, "%u frames high: overruns %u", (unsigned)extras[i], (unsigned)st.overruns);
    }
}

// A sender whose clock runs fast is still held back, short of an overrun.
static void test_a_fast_sender_is_trimmed(void) {
    sim_t s;
    start(&s);
    stream(&s, 240, 6);         // 0.3% fast
    btc_ring_stats_t st = stats(&s);
    CHECK(st.dropped > 0, "nothing dropped");
    CHECK(st.overruns == 0, "overruns %u", (unsigned)st.overruns);
    CHECK(s.high <= TARGET + CAPACITY / 4 + BURST * (PACKET + 6) + BLOCK, "fill reached %u", (unsigned)s.high);
}

// One whose clock runs slow is filled in, short of an underrun.
static void test_a_slow_sender_is_stretched(void) {
    sim_t s;
    start(&s);
    uint32_t every = BURST * PACKET / BLOCK;
    for (uint32_t i = 0; i < 240 * TICKS_A_SECOND; i++, s.tick++) {
        if (s.tick % every == 0) {
            send(&s, BURST * (PACKET - 6));
        }
        btc_ring_read(&s.ring, block, BLOCK, 2, 16129);
    }
    btc_ring_stats_t st = stats(&s);
    CHECK(st.repeated > 0, "nothing repeated");
    CHECK(st.underruns == 0, "underruns %u", (unsigned)st.underruns);
    CHECK(st.buffered >= TARGET - CAPACITY / 8 - BURST * PACKET - BLOCK, "fill fell to %u", (unsigned)st.buffered);
}

int main(void) {
    test_high_after_a_fast_start_drops_nothing();
    test_a_fast_sender_is_trimmed();
    test_a_slow_sender_is_stretched();
    if (failures) {
        printf("%d failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("ok\n");
    return EXIT_SUCCESS;
}
