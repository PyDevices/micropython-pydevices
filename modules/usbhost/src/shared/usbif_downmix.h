// SPDX-License-Identifier: MIT
//
// Stereo 16-bit PCM to mono, in place, for a host's capture stream: a webcam's
// microphone that only offers two channels, feeding a consumer that takes one.
// Each frame becomes the mean of its two samples, rounded toward minus
// infinity, so a full-scale signal on both channels stays full scale and
// nothing wraps. Done as each packet lands, in the packet's own buffer, so it
// costs no memory and no copy.
//
// Header-only and free of ESP-IDF so the host-side tests can check it.
#ifndef USBIF_DOWNMIX_H
#define USBIF_DOWNMIX_H

#include <stdint.h>

// `buf` holds `n` bytes of little-endian stereo s16 frames; on return its
// first bytes hold the mono frames. The mono byte count: n / 2, less any
// trailing partial frame.
static inline uint32_t usbif_downmix_s16(uint8_t *buf, uint32_t n) {
    const uint32_t frames = n / 4;
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t *in = buf + 4 * i;
        const int32_t l = (int16_t)(uint16_t)(in[0] | (in[1] << 8));
        const int32_t r = (int16_t)(uint16_t)(in[2] | (in[3] << 8));
        #ifndef USBIF_DOWNMIX_PLANT_LEFT_ONLY
        const int32_t m = (l + r) >> 1;
        #else
        const int32_t m = l;        // planted: the right channel is lost
        (void)r;
        #endif
        // Frame i's mono sample goes where its left sample was read from or
        // before it (2i <= 4i), so the loop never overwrites unread input.
        buf[2 * i] = (uint8_t)(m & 0xFF);
        buf[2 * i + 1] = (uint8_t)((m >> 8) & 0xFF);
    }
    return frames * 2;
}

#endif // USBIF_DOWNMIX_H
