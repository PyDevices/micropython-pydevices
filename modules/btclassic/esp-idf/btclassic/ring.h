// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio ring every btclassic profile plays through: 16-bit PCM written
// by the Bluetooth stack's task, read by the program at its output's pace.
// One writer and one reader, so it needs no lock. The two clocks never
// agree exactly, so the reader holds the ring near half full of its limit
// by dropping or repeating one frame now and then.
//
// Plain C with GCC's __atomic loads and stores (no compare-and-swap, so it
// builds for a Cortex-M0+ as well): any backend can use it.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btclassic.h"

typedef struct {
    int16_t *buf;               // capacity frames of `channels` samples
    uint32_t capacity;
    uint32_t limit;             // how much of it is in use, up to capacity
    uint8_t channels;           // 1 or 2
    // Positions count frames and wrap modulo 2^32; the index is pos % capacity.
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    volatile bool reset;        // set by the writer's side: start over from empty
    bool running;               // the reader's: false while it refills to half
    // The writer's counters.
    volatile uint32_t received, overruns;
    volatile uint32_t peak_l, peak_r;
    volatile bool peak_clear;   // set by the reader: the writer starts the peaks over
    // The reader's.
    volatile uint32_t underruns, dropped, repeated, skipped;
} btc_ring_t;

// Point the ring at `buf` and clear it. A NULL buf makes a ring that reads
// silence and takes nothing.
void btc_ring_init(btc_ring_t *r, int16_t *buf, uint32_t capacity, uint8_t channels);

// Use `limit` frames of the ring (a call holds less than music does), and
// start over from empty.
void btc_ring_set_limit(btc_ring_t *r, uint32_t limit);

// Start over from empty at the reader's next read: a new stream or format.
void btc_ring_restart(btc_ring_t *r);

// The writer: 16-bit PCM, interleaved when `in_channels` is 2. Mono is
// widened to a stereo ring; a mono ring keeps the left channel. Returns the
// frames taken: fewer when the ring is full, and that counts an overrun.
uint32_t btc_ring_write(btc_ring_t *r, const int16_t *in, uint32_t frames, uint8_t in_channels);

// The reader: fill `dst` with `frames` frames of `out_channels`, each sample
// scaled by gain / 16129 (16129 is unity: a 0..127 level squared). Silence
// fills whatever the ring can't. Returns how many frames came from the ring.
size_t btc_ring_read(btc_ring_t *r, int16_t *dst, size_t frames, uint8_t out_channels, int32_t gain);

void btc_ring_stats(const btc_ring_t *r, btc_ring_stats_t *out);

// The loudest samples written since the last call, 0..32767.
void btc_ring_levels(btc_ring_t *r, uint16_t *left, uint16_t *right);
