// SPDX-License-Identifier: MIT
//
// Host-side tests for the full-speed audio endpoint interval fix
// (workspace#16): a webcam's microphone says bInterval 4 for both speeds,
// which is 1 ms at high speed and 8 ms at full speed, and the P4's host polled
// it at the 8 ms. Only the audio streaming interface being claimed, at the
// alternate setting being claimed, has its isochronous endpoint changed.
#include "shared/usbhost_audio_interval.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static size_t put_itf(uint8_t *p, uint8_t n, uint8_t alt, uint8_t cls, uint8_t sub) {
    uint8_t d[9] = { 9, 4, n, alt, 1, cls, sub, 0, 0 };
    memcpy(p, d, 9);
    return 9;
}

static size_t put_ep(uint8_t *p, uint8_t addr, uint8_t attr, uint16_t wmax, uint8_t interval) {
    uint8_t d[7] = { 7, 5, addr, attr, (uint8_t)(wmax & 0xff), (uint8_t)(wmax >> 8), interval };
    memcpy(p, d, 7);
    return 7;
}

// Interface 1 is video (alt 1 an isochronous endpoint at bInterval 4, a
// legitimate slow one), interface 3 audio: alt 0 nothing, alt 1 the mic's
// isochronous endpoint at bInterval 4, alt 2 an interrupt endpoint at 4, and
// a class-specific descriptor (type 0x25) between them.
static size_t build(uint8_t *c, size_t *ep_video, size_t *ep_a1, size_t *ep_a2) {
    size_t n = 0;
    uint8_t cfg[9] = { 9, 2, 0, 0, 4, 1, 0, 0x80, 50 };
    memcpy(c, cfg, 9);
    n = 9;
    n += put_itf(c + n, 1, 1, 14, 2);
    *ep_video = n;
    n += put_ep(c + n, 0x81, 0x05, 944, 4);
    n += put_itf(c + n, 3, 0, 1, 2);
    n += put_itf(c + n, 3, 1, 1, 2);
    uint8_t cs[7] = { 7, 0x25, 1, 0, 0, 0, 0 };
    memcpy(c + n, cs, 7);
    n += 7;
    *ep_a1 = n;
    n += put_ep(c + n, 0x83, 0x05, 68, 4);
    n += put_itf(c + n, 3, 2, 1, 2);
    *ep_a2 = n;
    n += put_ep(c + n, 0x83, 0x03, 68, 4);
    c[2] = (uint8_t)n;
    return n;
}

int main(void) {
    uint8_t c[200];
    size_t v, a1, a2;
    size_t len = build(c, &v, &a1, &a2);

    // The mic's alt 1: its isochronous endpoint goes to every frame.
    CHECK(usbhost_audio_fs_interval_fix(c, len, 3, 1) == 1);
    CHECK(c[a1 + 6] == 1);
    // Nothing else moved: the video endpoint, and the other alternate's.
    CHECK(c[v + 6] == 4);
    CHECK(c[a2 + 6] == 4);

    // Claiming the video interface changes nothing: it isn't audio.
    len = build(c, &v, &a1, &a2);
    CHECK(usbhost_audio_fs_interval_fix(c, len, 1, 1) == 0);
    CHECK(c[v + 6] == 4 && c[a1 + 6] == 4);

    // An audio alternate with only an interrupt endpoint: not isochronous.
    CHECK(usbhost_audio_fs_interval_fix(c, len, 3, 2) == 0);
    CHECK(c[a2 + 6] == 4);

    // Alt 0 has no endpoints; an interface that isn't there changes nothing.
    CHECK(usbhost_audio_fs_interval_fix(c, len, 3, 0) == 0);
    CHECK(usbhost_audio_fs_interval_fix(c, len, 9, 1) == 0);
    CHECK(c[a1 + 6] == 4);

    // Already 1: left alone and not counted.
    c[a1 + 6] = 1;
    CHECK(usbhost_audio_fs_interval_fix(c, len, 3, 1) == 0);
    CHECK(c[a1 + 6] == 1);

    // A truncated descriptor stops at the end instead of reading past it.
    len = build(c, &v, &a1, &a2);
    CHECK(usbhost_audio_fs_interval_fix(c, a1 + 3, 3, 1) == 0);
    CHECK(c[a1 + 6] == 4);
    // A zero-length descriptor can't loop forever.
    c[a1] = 0;
    CHECK(usbhost_audio_fs_interval_fix(c, len, 3, 1) == 0);

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
