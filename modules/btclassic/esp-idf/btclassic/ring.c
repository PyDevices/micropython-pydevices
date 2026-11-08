// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio ring. See ring.h.

#include <string.h>

#include "ring.h"

static inline uint32_t load(const volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void store(volatile uint32_t *p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline uint16_t mag(int16_t v) {
    return v < 0 ? (uint16_t)(v == INT16_MIN ? 32767 : -v) : (uint16_t)v;
}

void btc_ring_init(btc_ring_t *r, int16_t *buf, uint32_t capacity, uint8_t channels) {
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->capacity = buf ? capacity : 0;
    r->limit = r->capacity;
    r->channels = channels == 1 ? 1 : 2;
}

void btc_ring_set_limit(btc_ring_t *r, uint32_t limit) {
    r->limit = limit < r->capacity ? limit : r->capacity;
    r->reset = true;
}

void btc_ring_restart(btc_ring_t *r) {
    r->reset = true;
}

uint32_t btc_ring_write(btc_ring_t *r, const int16_t *in, uint32_t frames, uint8_t in_channels) {
    if (r->buf == NULL) {
        return 0;
    }
    if (r->peak_clear) {
        r->peak_l = 0;
        r->peak_r = 0;
        r->peak_clear = false;
    }
    uint32_t wp = r->write_pos;
    uint32_t used = wp - load(&r->read_pos);
    uint32_t space = used < r->limit ? r->limit - used : 0;
    uint32_t n = frames < space ? frames : space;
    uint32_t pl = r->peak_l, pr = r->peak_r;
    for (uint32_t i = 0; i < n; i++) {
        int16_t left = in[i * in_channels];
        int16_t right = in_channels == 2 ? in[i * 2 + 1] : left;
        uint32_t at = (wp + i) % r->capacity;
        if (r->channels == 2) {
            r->buf[at * 2] = left;
            r->buf[at * 2 + 1] = right;
        } else {
            r->buf[at] = left;
        }
        uint16_t ml = mag(left), mr = mag(right);
        if (ml > pl) {
            pl = ml;
        }
        if (mr > pr) {
            pr = mr;
        }
    }
    store(&r->write_pos, wp + n);
    r->peak_l = pl;
    r->peak_r = pr;
    r->received += n;
    if (n < frames) {
        r->overruns++;
    }
    return n;
}

size_t btc_ring_read(btc_ring_t *r, int16_t *dst, size_t frames, uint8_t out_channels, int32_t gain) {
    size_t width = out_channels == 1 ? 1 : 2;
    if (r->buf == NULL) {
        memset(dst, 0, frames * width * sizeof(int16_t));
        return 0;
    }
    if (r->reset) {
        r->reset = false;
        store(&r->read_pos, load(&r->write_pos));
        r->running = false;
    }
    uint32_t rp = r->read_pos;
    uint32_t avail = load(&r->write_pos) - rp;
    uint32_t target = r->limit / 2;
    uint32_t slack = r->limit / 8;

    if (!r->running) {
        if (avail < target) {
            memset(dst, 0, frames * width * sizeof(int16_t));
            return 0;
        }
        r->running = true;
        // Play from the fill point. A reader that came late while the ring
        // refilled (a stream that starts as the program decodes a cover)
        // finds it well above, and at one frame a read the excess takes
        // minutes to drain; meanwhile any short stall overflows the ring and
        // loses packets. Skipping the oldest costs the first moment of the
        // stream, once. Only here: in steady state a program that keeps a
        // deep output queue reads its backlog back after a stall, and a skip
        // then would throw away audio about to play.
        if (avail > target + slack) {
            uint32_t skip = avail - target;
            rp += skip;
            avail = target;
            r->skipped += skip;
        }
    }

    // Take one frame more or less than asked for when the ring has drifted
    // away from half full: the sender's clock and ours never quite agree.
    size_t take = frames;
    int adjust = 0;
    if (avail > target + slack && avail > frames + 1) {
        take = frames + 1;
        adjust = 1;
    } else if (avail < target - slack && frames > 1) {
        take = frames - 1;
        adjust = -1;
    }
    size_t from_ring = take <= avail ? take : avail;

    uint8_t ch = r->channels;
    size_t out = 0;
    for (size_t i = 0; i < from_ring && out < frames; i++) {
        if (adjust > 0 && i == from_ring / 2) {
            continue;   // drop this frame
        }
        const int16_t *src = &r->buf[((rp + i) % r->capacity) * ch];
        int16_t *o = &dst[out * width];
        o[0] = (int16_t)(((int32_t)src[0] * gain) / 16129);
        if (width == 2) {
            o[1] = (int16_t)(((int32_t)src[ch - 1] * gain) / 16129);
        }
        out++;
    }
    size_t played = out;
    if (adjust < 0 && out > 0 && out < frames) {
        memcpy(&dst[out * width], &dst[(out - 1) * width], width * sizeof(int16_t));  // repeat the last frame
        out++;
    }
    store(&r->read_pos, rp + from_ring);

    if (out < frames) {
        r->underruns++;
        memset(&dst[out * width], 0, (frames - out) * width * sizeof(int16_t));
        r->running = false;  // ran dry: refill to half before playing on
    } else if (adjust > 0) {
        r->dropped++;
    } else if (adjust < 0) {
        r->repeated++;
    }
    return played;
}

void btc_ring_stats(const btc_ring_t *r, btc_ring_stats_t *out) {
    out->buffered = load(&r->write_pos) - load(&r->read_pos);
    out->capacity = r->capacity;
    out->received = r->received;
    out->underruns = r->underruns;
    out->overruns = r->overruns;
    out->dropped = r->dropped;
    out->repeated = r->repeated;
    out->skipped = r->skipped;
}

void btc_ring_levels(btc_ring_t *r, uint16_t *left, uint16_t *right) {
    *left = (uint16_t)r->peak_l;
    *right = (uint16_t)r->peak_r;
    r->peak_clear = true;
}
