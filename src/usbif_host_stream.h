// SPDX-License-Identifier: MIT
//
// Isochronous host streams (usbif_host_stream.c): C moves a hosted device's
// isochronous packets between an endpoint and a ring, so audio and video
// never wait on the interpreter. The class logic -- which alternate setting,
// the rate, the camera's negotiation -- stays in Python (usb.host.audio,
// usb.host.video) over usb.core's control transfers; a stream is opened on
// an endpoint whose interface Python has already claimed at its streaming
// alternate.
//
// The ring is reached through calls, so the stream knows no MicroPython
// types: mod_usbif.c hands it a USBDevice.Ring's C surface (the same ring a
// device-side stream uses) or an audiopump.Ring's sink, for a microphone
// that plays straight through audiodsp's pump.
#ifndef USBIF_HOST_STREAM_H
#define USBIF_HOST_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pcm_c_sink.h"

// A playback stream, a capture stream and a camera at once, with one spare.
#define USBIF_HOST_STREAMS (4)

typedef struct {
    void *ctx;
    // IN: packets in. Bytes taken (all or nothing for a USBDevice.Ring,
    // whole frames as fit for an audiopump.Ring).
    uint32_t (*put)(void *ctx, const uint8_t *src, uint32_t n);
    // OUT: bytes out, and what's waiting. NULL for a ring that only takes.
    uint32_t (*get)(void *ctx, uint8_t *dst, uint32_t n);
    uint32_t (*used)(void *ctx);
    uint32_t (*size)(void *ctx);
    // Video: a frame built in place and published whole (usbif_uvc_frame.h).
    bool (*stage)(void *ctx, uint32_t offset, const uint8_t *src, uint32_t n);
    void (*commit)(void *ctx, uint32_t n);
    void (*overflow)(void *ctx);
} usbif_stream_ring_t;

typedef struct {
    usbif_stream_ring_t ring;
    uint16_t packet;        // bytes a packet; 0: the endpoint's own (with mult)
    // OUT: PCM at `rate` in `frame`-byte frames, an interval's worth a
    // packet; rate 0 sends what the ring has, up to `packet`. `frame` also
    // keeps writes and reads to whole frames.
    uint32_t rate;
    uint16_t frame;
    // IN: UVC payloads into whole frames, each led in the ring by its length.
    bool video;
    // IN: stereo 16-bit PCM averaged to mono as each packet lands.
    bool downmix;
    uint32_t max_frame;     // video: bigger frames are dropped; 0 for any
} usbif_stream_cfg_t;

typedef struct {
    uint32_t packets;       // packets that carried data
    uint32_t bytes;
    uint32_t dropped;       // IN: packets or frames the ring had no room for
    uint32_t starved;       // OUT: packets padded with silence, the ring short
    uint32_t errors;        // packets the bus failed
    uint32_t empty;         // IN: packets that completed carrying nothing
    uint32_t frames;        // video: whole frames into the ring
    uint32_t torn;          // video: frames dropped torn
    uint32_t big;           // video: frames dropped for size
} usbif_stream_stats_t;

// Each returns a negative MicroPython errno on failure.

// Start a stream on `ep`, whose interface must be claimed at an alternate
// setting that carries it (usbif_xfer_claim). The slot, 0..STREAMS-1.
int usbif_stream_open(uint32_t dev_id, uint8_t ep, const usbif_stream_cfg_t *cfg);
// Stop it and free its transfers; the ring is the caller's again once this
// returns. The slot it was in; -ENOENT if there was none.
int usbif_stream_close(uint32_t dev_id, uint8_t ep);
// The slot, or -ENOENT.
int usbif_stream_stats(uint32_t dev_id, uint8_t ep, usbif_stream_stats_t *out);
// OUT: the interpreter's write, through the same gate as a C sink's.
int usbif_stream_write(uint32_t dev_id, uint8_t ep, const uint8_t *data, size_t len);
int usbif_stream_space(uint32_t dev_id, uint8_t ep);
// OUT: a pcm_c_sink_t another native module writes from its own task.
int usbif_stream_c_sink(uint32_t dev_id, uint8_t ep, pcm_c_sink_t *out, uint32_t channels, uint32_t bits);

// For the host core and the transfer layer, with the host lock held.
void usbif_stream_stop_itf(void *dev, int itf, bool device_gone);
void usbif_stream_close_all(void);
// Bit i set: slot i holds no stream. mod_usbif.c lets go of its rings by it.
uint32_t usbif_stream_free_slots(void);

#endif // USBIF_HOST_STREAM_H
