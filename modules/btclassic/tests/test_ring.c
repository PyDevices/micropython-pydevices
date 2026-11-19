// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The A2DP sink's ring, fed the way a phone feeds it: packets in bursts, read
// a block at a time at the output's pace, at each size AudioSink makes. Runs
// on the desktop.
//
// From modules/btclassic:
//   cc -std=c11 -Wall -Wextra -Werror -Iesp-idf/btclassic -Iesp-idf/btclassic/include
//      -o /tmp/test_ring tests/test_ring.c esp-idf/btclassic/ring.c

#include <stdio.h>
#include <stdlib.h>

#include "ring.h"

#define MAX_FRAMES 48000u
#define BLOCK 256u              // frames a read takes
#define PACKET 2048u            // frames a packet carries at most
#define SECONDS 240u
#define ORDINARY_BURST_MS 81u   // what a phone's bursts usually swing the fill by
#define LARGEST_BURST_MS 93u    // rounded up to whole reads: the most it was seen to
#define PEAK_ABOVE_HALF 1728u   // how far above half full a burst carries the fill
#define FAST 3000u              // parts per million: far beyond any real clock
#define FASTEST 3500u           // near the most a frame a block can take out (3906)

// AudioSink makes 48 frames per ms of buffer_ms whatever the stream's rate.
static const uint32_t sizes[] = {48 * 250, 48 * 500, 48 * 1000};
static const uint32_t rates[] = {44100, 48000};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int16_t storage[MAX_FRAMES * 2];
static int16_t packet[PACKET * 2];
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
    uint32_t burst;             // frames the sender sends at a time
    int32_t ppm;                // how far its clock is from ours
    int64_t remainder;
    uint64_t played, next_burst;
    uint32_t high, low;         // the fill's extremes
} sim_t;

static void send(sim_t *s, uint32_t frames) {
    while (frames) {
        uint32_t n = frames > PACKET ? PACKET : frames;
        btc_ring_write(&s->ring, packet, n, 2);
        frames -= n;
    }
}

static void stream(sim_t *s, uint32_t rate, uint32_t seconds) {
    uint64_t end = s->played + (uint64_t)rate * seconds;
    while (s->played < end) {
        if (s->played >= s->next_burst) {
            int64_t scaled = (int64_t)s->burst * (1000000 + s->ppm) + s->remainder;
            send(s, (uint32_t)(scaled / 1000000));
            s->remainder = scaled % 1000000;
            s->next_burst += s->burst;
        }
        btc_ring_read(&s->ring, block, BLOCK, 2, 16129);
        s->played += BLOCK;
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

// Playing from half full, the fill swinging around it as it does on a board:
// the first burst comes once enough has been read for it to peak where a
// phone's do.
static void start(sim_t *s, uint32_t size, uint32_t burst, int32_t ppm) {
    btc_ring_init(&s->ring, storage, size, 2);
    s->burst = burst;
    s->ppm = ppm;
    s->remainder = 0;
    s->played = 0;
    s->next_burst = burst > PEAK_ABOVE_HALF ? burst - PEAK_ABOVE_HALF + BLOCK - 1 : 0;
    s->next_burst -= s->next_burst % BLOCK;
    send(s, size / 2);
    s->high = 0;
    s->low = size;
}

static btc_ring_stats_t stats(const sim_t *s) {
    btc_ring_stats_t st;
    btc_ring_stats(&s->ring, &st);
    return st;
}

// In whole reads, so each burst lands at the same point of the swing.
static uint32_t burst_frames(uint32_t rate, uint32_t ms) {
    return (rate * ms / 1000 + BLOCK - 1) / BLOCK * BLOCK;
}

// A fast start leaves the ring high once it is playing, and on a small ring
// the bursts alone reach as far. The clocks agree, so nothing needs correcting
// and no frame may be lost to them.
static void test_ordinary_bursts_drop_nothing(void) {
    static const struct { uint32_t size, high; } cases[] = {
        {48 * 250, 0},
        {48 * 500, 1000}, {48 * 500, 2000}, {48 * 500, 3000},
        {48 * 1000, 2000}, {48 * 1000, 4000}, {48 * 1000, 5000}, {48 * 1000, 6000},
    };
    for (size_t i = 0; i < COUNT(cases); i++) {
        static sim_t s;
        start(&s, cases[i].size, burst_frames(44100, ORDINARY_BURST_MS), 0);
        send(&s, cases[i].high);
        stream(&s, 44100, 60);
        btc_ring_stats_t st = stats(&s);
        CHECK(st.dropped == 0, "%u frames, %u high: dropped %u",
            (unsigned)cases[i].size, (unsigned)cases[i].high, (unsigned)st.dropped);
        CHECK(st.overruns == 0, "%u frames, %u high: overruns %u",
            (unsigned)cases[i].size, (unsigned)cases[i].high, (unsigned)st.overruns);
    }
}

// A sender whose clock runs fast is held back with room left for its largest
// burst: an overrun loses a whole packet.
static void test_a_fast_sender_never_overruns(void) {
    static const uint32_t speeds[] = {FAST, FASTEST};
    for (size_t i = 0; i < COUNT(sizes); i++) {
        for (size_t j = 0; j < COUNT(rates); j++) {
            for (size_t k = 0; k < COUNT(speeds); k++) {
                static sim_t s;
                start(&s, sizes[i], burst_frames(rates[j], LARGEST_BURST_MS), (int32_t)speeds[k]);
                stream(&s, rates[j], SECONDS);
                btc_ring_stats_t st = stats(&s);
                CHECK(st.dropped > 0, "%u frames at %u Hz, +%u ppm: nothing dropped",
                    (unsigned)sizes[i], (unsigned)rates[j], (unsigned)speeds[k]);
                CHECK(st.overruns == 0, "%u frames at %u Hz, +%u ppm: overruns %u, fill reached %u",
                    (unsigned)sizes[i], (unsigned)rates[j], (unsigned)speeds[k],
                    (unsigned)st.overruns, (unsigned)s.high);
            }
        }
    }
}

// One whose clock runs slow is filled in, short of an underrun.
static void test_a_slow_sender_is_stretched(void) {
    for (size_t i = 0; i < COUNT(sizes); i++) {
        static sim_t s;
        start(&s, sizes[i], burst_frames(44100, LARGEST_BURST_MS), -(int32_t)FAST);
        stream(&s, 44100, SECONDS);
        btc_ring_stats_t st = stats(&s);
        CHECK(st.repeated > 0, "%u frames: nothing repeated", (unsigned)sizes[i]);
        CHECK(st.underruns == 0, "%u frames: underruns %u", (unsigned)sizes[i], (unsigned)st.underruns);
    }
}

// A call's ring is too small to spare any room: it trims an eighth above half.
static void test_a_small_ring_trims_at_an_eighth(void) {
    static const uint32_t small[] = {3200, 6400};
    for (size_t i = 0; i < COUNT(small); i++) {
        static sim_t s;
        start(&s, small[i], BLOCK, 0);
        send(&s, small[i] / 8 - 2 * BLOCK);
        stream(&s, 16000, 1);
        CHECK(stats(&s).dropped == 0, "%u frames: dropped below the edge", (unsigned)small[i]);
        send(&s, 3 * BLOCK);
        stream(&s, 16000, 1);
        CHECK(stats(&s).dropped > 0, "%u frames: nothing dropped above the edge", (unsigned)small[i]);
    }
}

int main(void) {
    test_ordinary_bursts_drop_nothing();
    test_a_fast_sender_never_overruns();
    test_a_slow_sender_is_stretched();
    test_a_small_ring_trims_at_an_eighth();
    if (failures) {
        printf("%d failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("ok\n");
    return EXIT_SUCCESS;
}
