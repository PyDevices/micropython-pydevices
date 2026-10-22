// SPDX-License-Identifier: MIT
//
// The poll interval of a full-speed audio stream's isochronous endpoint.
//
// An isochronous endpoint's bInterval is an exponent: 2^(bInterval-1) bus
// slots, which is microframes (125 us) at high speed and frames (1 ms) at
// full speed. A webcam's microphone has one configuration for both speeds and
// says bInterval 4: eight microframes, 1 ms, at high speed, but eight frames,
// 8 ms, at full speed. ESP-IDF's host driver polls what the descriptor says,
// so behind a hub that runs the P4's host at full speed the microphone is
// polled once in eight packets and a 16 kHz stereo stream arrives as 126
// packets a second instead of 999 (workspace#16).
//
// USB Audio 1.0 (4.6.1.1) and 2.0 give an audio streaming endpoint a period
// of one frame at full speed, and a host that serves audio, Linux's among
// them, polls every frame whatever the field says. This sets the field to 1
// for the audio streaming interface about to be claimed, in the copy of the
// configuration descriptor the host library keeps, before the library reads
// it to open the pipes.
//
// Header-only and free of ESP-IDF so the host-side tests can check it.
#ifndef USBHOST_AUDIO_INTERVAL_H
#define USBHOST_AUDIO_INTERVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USBHOST_AI_DT_INTERFACE   (4)
#define USBHOST_AI_DT_ENDPOINT    (5)
#define USBHOST_AI_CLASS_AUDIO    (1)
#define USBHOST_AI_SUBCLASS_AS    (2)   // AudioStreaming
#define USBHOST_AI_XFER_ISOC      (1)

// Walk the configuration descriptor `cfg` (`len` bytes) and set bInterval to 1
// on every isochronous endpoint of interface `itf`, alternate setting `alt`,
// when that interface is an AudioStreaming one. Returns the number of
// endpoints changed. The caller has already checked the bus is full speed.
static inline int usbhost_audio_fs_interval_fix(uint8_t *cfg, size_t len, uint8_t itf, uint8_t alt) {
    int changed = 0;
    bool ours = false;
    size_t off = 0;
    while (off + 2 <= len && cfg[off] >= 2 && off + cfg[off] <= len) {
        uint8_t *d = cfg + off;
        if (d[1] == USBHOST_AI_DT_INTERFACE && d[0] >= 9) {
            ours = d[2] == itf && d[3] == alt
                && d[5] == USBHOST_AI_CLASS_AUDIO && d[6] == USBHOST_AI_SUBCLASS_AS;
        } else if (d[1] == USBHOST_AI_DT_ENDPOINT && d[0] >= 7 && ours
            && (d[3] & 0x03) == USBHOST_AI_XFER_ISOC && d[6] > 1) {
#ifndef USBHOST_AI_PLANT_NO_FIX
            d[6] = 1;
#endif
            changed++;
        }
        off += d[0];
    }
    return changed;
}

#endif // USBHOST_AUDIO_INTERVAL_H
