// SPDX-License-Identifier: MIT
//
// Host-side tests for the generic pieces of a USB host's isochronous
// streams: PCM pacing (usbif_iso_pace.h) and UVC payloads into whole frames
// (usbif_uvc_frame.h), driven here against a fake ring.
//
//   cc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/t tests/test_iso_stream.c && /tmp/t

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shared/usbif_iso_pace.h"
#include "shared/usbif_uvc_frame.h"

static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
} while (0)

// --- pacing ------------------------------------------------------------------

static void test_pace(uint32_t rate, uint32_t frame, uint32_t slot_us, uint32_t max,
    uint32_t slots, uint32_t lo, uint32_t hi) {
    usbif_iso_pace_t p;
    usbif_iso_pace_init(&p, rate, frame, slot_us);
    uint64_t total = 0;
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t n = usbif_iso_pace_next(&p, max);
        CHECK(n % frame == 0, "rate %u: packet %u bytes is not whole frames", (unsigned)rate, (unsigned)n);
        CHECK(n <= max, "rate %u: packet %u over max %u", (unsigned)rate, (unsigned)n, (unsigned)max);
        CHECK(n >= lo * frame && n <= hi * frame, "rate %u: packet of %u frames, outside %u..%u",
            (unsigned)rate, (unsigned)(n / frame), (unsigned)lo, (unsigned)hi);
        total += n;
    }
    // Over a whole second the frames sent are the rate, exactly.
    uint64_t want = (uint64_t)rate * frame * slots * slot_us / 1000000u;
    CHECK(total == want, "rate %u slot %u us: %llu bytes in %u slots, want %llu", (unsigned)rate,
        (unsigned)slot_us, (unsigned long long)total, (unsigned)slots, (unsigned long long)want);
}

static void test_pacing(void) {
    test_pace(48000, 4, 1000, 196, 1000, 48, 48);      // full speed, 48 kHz stereo 16-bit
    test_pace(44100, 4, 1000, 196, 1000, 44, 45);      // 44.1: 44 then 45
    test_pace(48000, 4, 125, 196, 8000, 6, 6);         // high speed, every microframe
    test_pace(44100, 6, 1000, 288, 1000, 44, 45);      // 24-bit stereo
    test_pace(96000, 8, 1000, 1024, 1000, 96, 96);
    // A cap below an interval's frames: capped to whole frames.
    usbif_iso_pace_t p;
    usbif_iso_pace_init(&p, 48000, 4, 1000);
    uint32_t n = usbif_iso_pace_next(&p, 102);
    CHECK(n == 100, "capped packet %u, want 100", (unsigned)n);
}

// --- frames ------------------------------------------------------------------

#define RING_SIZE (256)

typedef struct {
    uint8_t buf[RING_SIZE];
    uint32_t head, tail;        // free-running
    uint32_t overflows;
    bool refuse;                // stage() always fails
} fake_ring_t;

static bool fake_stage(void *r, uint32_t offset, const uint8_t *src, uint32_t n) {
    fake_ring_t *f = r;
    uint32_t used = f->head - f->tail;
    if (f->refuse || offset + n > RING_SIZE - used) {
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        f->buf[(f->head + offset + i) % RING_SIZE] = src[i];
    }
    return true;
}

static void fake_commit(void *r, uint32_t n) {
    ((fake_ring_t *)r)->head += n;
}

static void fake_overflow(void *r) {
    ((fake_ring_t *)r)->overflows++;
}

// The next record: its payload into out, the length, or -1 when empty.
static int fake_pop(fake_ring_t *f, uint8_t *out) {
    if (f->head - f->tail < 4) {
        return -1;
    }
    uint32_t len = 0;
    for (int i = 0; i < 4; i++) {
        len |= (uint32_t)f->buf[(f->tail + i) % RING_SIZE] << (8 * i);
    }
    for (uint32_t i = 0; i < len; i++) {
        out[i] = f->buf[(f->tail + 4 + i) % RING_SIZE];
    }
    f->tail += 4 + len;
    return (int)len;
}

static fake_ring_t ring;
static usbif_uvc_frame_t fr;

static void frame_setup(uint32_t max_frame) {
    memset(&ring, 0, sizeof(ring));
    memset(&fr, 0, sizeof(fr));
    fr.ring = &ring;
    fr.stage = fake_stage;
    fr.commit = fake_commit;
    fr.overflow = fake_overflow;
    fr.max_frame = max_frame;
    usbif_uvc_frame_start(&fr);
}

// A packet: a 2-byte header with these flags, then n bytes of `v`.
static void pkt(uint8_t flags, uint32_t n, uint8_t v) {
    uint8_t p[64];
    p[0] = 2;
    p[1] = flags;
    memset(p + 2, v, n);
    usbif_uvc_frame_payload(&fr, p, 2 + n);
}

static void test_frames(void) {
    uint8_t out[RING_SIZE];
    const uint8_t F0 = 0, F1 = USBIF_UVC_HDR_FID, EOF_ = USBIF_UVC_HDR_EOF;

    // Started mid-frame: that frame is dropped, the next is whole.
    frame_setup(0);
    pkt(F0, 10, 1);
    pkt(F0 | EOF_, 5, 1);
    CHECK(fr.frames == 0 && fr.dropped_torn == 1, "first frame: frames %u torn %u",
        (unsigned)fr.frames, (unsigned)fr.dropped_torn);
    CHECK(fake_pop(&ring, out) == -1, "a partial first frame reached the ring");
    pkt(F1, 20, 2);
    pkt(F1, 20, 3);
    pkt(F1 | EOF_, 3, 4);
    CHECK(fr.frames == 1, "frames %u, want 1", (unsigned)fr.frames);
    int n = fake_pop(&ring, out);
    CHECK(n == 43, "frame of %d bytes, want 43", n);
    CHECK(n == 43 && out[0] == 2 && out[20] == 3 && out[42] == 4, "frame bytes out of order");

    // Back to back, with the frame ID toggling each time.
    pkt(F0, 8, 5);
    pkt(F0 | EOF_, 0, 0);       // an empty end-of-frame packet
    pkt(F1 | EOF_, 4, 6);
    CHECK(fr.frames == 3, "frames %u, want 3", (unsigned)fr.frames);
    CHECK(fake_pop(&ring, out) == 8 && fake_pop(&ring, out) == 4, "two frames back to back");

    // The frame ID moves on without an end of frame: the frame is torn.
    pkt(F0, 10, 7);
    pkt(F1, 10, 8);             // last frame's EOF went missing
    pkt(F1 | EOF_, 2, 9);
    CHECK(fr.dropped_torn == 2 && fr.frames == 4, "missing EOF: torn %u frames %u",
        (unsigned)fr.dropped_torn, (unsigned)fr.frames);
    n = fake_pop(&ring, out);
    CHECK(n == 12 && out[0] == 8 && out[11] == 9, "the frame after a missing EOF: %d bytes", n);

    // An error bit tears the frame it is in, and only that one.
    pkt(F0, 10, 1);
    pkt(F0 | USBIF_UVC_HDR_ERR, 10, 1);
    pkt(F0 | EOF_, 10, 1);
    CHECK(fr.dropped_torn == 3, "error bit: torn %u", (unsigned)fr.dropped_torn);
    pkt(F1 | EOF_, 6, 2);
    CHECK(fake_pop(&ring, out) == 6, "the frame after an error");

    // A bad header length tears it too.
    uint8_t bad[4] = { 9, F0, 0, 0 };
    usbif_uvc_frame_payload(&fr, bad, 4);
    pkt(F0 | EOF_, 4, 1);
    CHECK(fr.dropped_torn == 4, "bad header: torn %u", (unsigned)fr.dropped_torn);

    // No room: dropped whole and counted as full, never published in part.
    uint32_t before = fr.frames;
    pkt(F1, 60, 1);
    pkt(F1, 60, 1);
    pkt(F1, 60, 1);
    pkt(F1, 60, 1);             // 240 + 4 > what's left
    pkt(F1 | EOF_, 60, 1);
    CHECK(fr.dropped_full == 1 && ring.overflows == 1 && fr.frames == before,
        "full: dropped %u overflows %u", (unsigned)fr.dropped_full, (unsigned)ring.overflows);
    CHECK(fake_pop(&ring, out) == -1, "a frame with no room reached the ring");
    pkt(F0 | EOF_, 10, 3);
    CHECK(fake_pop(&ring, out) == 10, "the frame after a full one");

    // Bigger than allowed: dropped as big, not torn.
    frame_setup(30);
    pkt(F0 | EOF_, 1, 0);       // first boundary
    pkt(F1, 20, 1);
    pkt(F1 | EOF_, 20, 1);
    CHECK(fr.dropped_big == 1 && fr.dropped_torn == 1 && fr.frames == 0, "big: big %u torn %u",
        (unsigned)fr.dropped_big, (unsigned)fr.dropped_torn);
    pkt(F0 | EOF_, 30, 2);
    CHECK(fr.frames == 1 && fake_pop(&ring, out) == 30, "a frame of exactly max_frame");

    // A ring that takes nothing: every frame dropped, none published.
    frame_setup(0);
    ring.refuse = true;
    pkt(F0 | EOF_, 1, 0);
    for (int i = 0; i < 5; i++) {
        pkt((i & 1 ? F0 : F1) | EOF_, 10, 1);
    }
    CHECK(fr.frames == 0 && fr.dropped_full == 5, "refusing ring: frames %u full %u",
        (unsigned)fr.frames, (unsigned)fr.dropped_full);
}

int main(void) {
    test_pacing();
    test_frames();
    if (failures) {
        printf("iso stream: %d FAILED\n", failures);
        return 1;
    }
    printf("iso stream: all checks passed\n");
    return 0;
}
