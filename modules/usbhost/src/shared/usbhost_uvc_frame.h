// SPDX-License-Identifier: MIT
//
// UVC payloads into whole frames, for a host's isochronous video stream.
//
// Every packet a camera sends begins with a payload header: its length, then
// a flags byte carrying the frame ID (toggled between frames), end of frame
// and error. A frame is only useful whole -- half a JPEG is not a smaller
// JPEG -- so this builds each frame in the ring without publishing it, and
// commits it, led by its length (4 bytes, little-endian), only once its last
// packet is in. A frame torn on the bus (an error bit, a frame ID that toggled
// before its end), one the ring had no room for, and one larger than allowed
// are each dropped whole and counted apart. The first frame after a start is
// dropped too: the stream may have begun in its middle.
//
// The same record a device-side UVC stream reads (machine.USBDevice
// stream(..., frame=-1)), so a host can hand frames straight to a device.
//
// Header-only and free of ESP-IDF; the ring is reached through three calls,
// so the host-side tests drive it with a fake.
#ifndef USBHOST_UVC_FRAME_H
#define USBHOST_UVC_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#define USBHOST_UVC_HDR_FID (0x01)
#define USBHOST_UVC_HDR_EOF (0x02)
#define USBHOST_UVC_HDR_ERR (0x40)
#define USBHOST_UVC_LEN_BYTES (4)

typedef struct {
    // The ring: stage n bytes at offset past its head (false: no room),
    // publish the first n staged, count a record dropped for want of room.
    void *ring;
    bool (*stage)(void *ring, uint32_t offset, const uint8_t *src, uint32_t n);
    void (*commit)(void *ring, uint32_t n);
    void (*overflow)(void *ring);
    uint32_t max_frame;     // 0: any size the ring holds

    uint32_t fill;          // payload bytes of this frame staged
    uint32_t seen;          // payload bytes of this frame received
    uint32_t got;           // packets of this frame with a good header
    bool has_fid;
    uint8_t fid;
    bool torn, full, big;

    uint32_t frames, dropped_full, dropped_torn, dropped_big;
} usbhost_uvc_frame_t;

static inline void usbhost_uvc_frame_start(usbhost_uvc_frame_t *f) {
    f->fill = f->seen = f->got = 0;
    f->has_fid = false;
    f->fid = 0;
    #ifndef USBHOST_UVC_FRAME_PLANT_NO_SYNC
    f->torn = true;         // until the first frame boundary
    #else
    f->torn = false;        // planted: publish a frame the stream began inside
    #endif
    f->full = f->big = false;
    f->frames = f->dropped_full = f->dropped_torn = f->dropped_big = 0;
}

static inline void usbhost_uvc_frame_end(usbhost_uvc_frame_t *f) {
    if (f->big) {
        f->dropped_big++;
    } else if (f->torn) {
        if (f->seen) {
            f->dropped_torn++;
        }
    } else if (f->full) {
        f->dropped_full++;
        f->overflow(f->ring);
    } else if (f->fill) {
        const uint8_t len[USBHOST_UVC_LEN_BYTES] = {
            (uint8_t)f->fill, (uint8_t)(f->fill >> 8), (uint8_t)(f->fill >> 16), (uint8_t)(f->fill >> 24),
        };
        // Room for these four was there when the payload after them fit.
        f->stage(f->ring, 0, len, USBHOST_UVC_LEN_BYTES);
        f->commit(f->ring, USBHOST_UVC_LEN_BYTES + f->fill);
        f->frames++;
    }
    f->fill = f->seen = f->got = 0;
    f->torn = f->full = f->big = false;
}

// One packet as it arrived (header included).
static inline void usbhost_uvc_frame_payload(usbhost_uvc_frame_t *f, const uint8_t *data, uint32_t len) {
    if (len < 2) {
        return;
    }
    const uint8_t hlen = data[0];
    const uint8_t flags = data[1];
    if (hlen < 2 || hlen > len) {
        f->torn = true;
        return;
    }
    const uint8_t fid = flags & USBHOST_UVC_HDR_FID;
    if (!f->has_fid) {
        f->has_fid = true;
        f->fid = fid;
    } else if (fid != f->fid) {
        f->fid = fid;
        if (f->got) {
            // The frame ID moved on without an end of frame: the last
            // frame's final packet went missing, so it is torn, and this
            // one begins. (Straight after an end of frame, a new ID is
            // simply the next frame.)
            f->torn = true;
            usbhost_uvc_frame_end(f);
        }
    }
    f->got++;
    if (flags & USBHOST_UVC_HDR_ERR) {
        f->torn = true;
    }
    const uint32_t n = len - hlen;
    if (n) {
        f->seen += n;
        if (f->max_frame && f->seen > f->max_frame) {
            f->big = true;
        }
        if (!f->torn && !f->full && !f->big) {
            if (f->stage(f->ring, USBHOST_UVC_LEN_BYTES + f->fill, data + hlen, n)) {
                f->fill += n;
            } else {
                f->full = true;
            }
        }
    }
    if (flags & USBHOST_UVC_HDR_EOF) {
        usbhost_uvc_frame_end(f);
        // The next packet starts a frame of its own; a stream that began
        // mid-frame is in step from here.
    }
}

#endif // USBHOST_UVC_FRAME_H
