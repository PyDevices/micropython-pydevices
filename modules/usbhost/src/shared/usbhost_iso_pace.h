// SPDX-License-Identifier: MIT
//
// How many bytes a host's isochronous OUT packet carries when the stream is
// PCM at a fixed rate: the frames one bus interval holds, the fraction
// carried to the next. 44.1 kHz on a full-speed bus sends 44 frames nine
// times and 45 the tenth; 48 kHz on a high-speed endpoint polled every
// microframe sends 6.
//
// A packet as large as the endpoint allows, every interval, is not the same
// thing. A 2.0 device sizes its maximum packet at an interval's frames plus
// one, so sending the maximum runs about 2 % fast and the device drops what it
// cannot hold.
//
// Header-only and free of ESP-IDF so the host-side tests can check it.
#ifndef USBHOST_ISO_PACE_H
#define USBHOST_ISO_PACE_H

#include <stdint.h>

typedef struct {
    uint32_t rate;      // frames a second
    uint32_t frame;     // bytes a frame (channels x sample bytes)
    uint32_t slot_us;   // the endpoint's period: interval x 1000 (full speed) or x 125 (high)
    uint64_t acc;       // frame-microseconds owed, below one frame
} usbhost_iso_pace_t;

static inline void usbhost_iso_pace_init(usbhost_iso_pace_t *p, uint32_t rate, uint32_t frame,
    uint32_t slot_us) {
    p->rate = rate;
    p->frame = frame ? frame : 1;
    p->slot_us = slot_us ? slot_us : 1000;
    p->acc = 0;
}

// The next packet's bytes, whole frames, never more than `max` allows.
static inline uint32_t usbhost_iso_pace_next(usbhost_iso_pace_t *p, uint32_t max) {
    p->acc += (uint64_t)p->rate * p->slot_us;
    const uint32_t frames = (uint32_t)(p->acc / 1000000u);
    p->acc -= (uint64_t)frames * 1000000u;
    #ifdef USBHOST_ISO_PACE_PLANT_DROP_REMAINDER
    p->acc = 0;             // planted: 44.1 kHz then sends 44 frames every time
    #endif
    uint32_t want = frames * p->frame;
    const uint32_t cap = max - max % p->frame;
    return want > cap ? cap : want;
}

#endif // USBHOST_ISO_PACE_H
