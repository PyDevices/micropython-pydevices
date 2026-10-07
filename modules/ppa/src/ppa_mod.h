// ppa: scale, rotate, mirror, colour-convert, fill and blend on pixel buffers.
// On the ESP32-P4 its Pixel Processing Accelerator does the work; elsewhere
// (and on request) portable C does scale, rotate, mirror and colour convert.
//
// The C API, for other modules (h264enc's colour conversion, cameraif's
// capture_scaled and capture_yuy2). A module that may be built without ppa
// declares these weak and checks ppa_mod_open for NULL. Each caller opens its
// own client: the PPA queues one operation per client, and h264enc's runs on
// a core-0 task while Python's run on the interpreter's.
#ifndef PPA_MOD_H
#define PPA_MOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pixel formats, as the P4 lays them out in memory. RGB565 widens to 8 bits
// by zero-filling (31 -> 248), as the PPA does it; the software path matches.
enum {
    PPA_MOD_RGB565 = 0,     // 16-bit, native (little-endian); swap for big-endian input
    PPA_MOD_RGB888 = 1,     // B, G, R bytes
    PPA_MOD_ARGB8888 = 2,   // B, G, R, A bytes
    PPA_MOD_GRAY8 = 3,      // one byte
    PPA_MOD_YUY2 = 4,       // Y0 U Y1 V per pixel pair (YUYV)
    PPA_MOD_UYVY = 5,       // U Y0 V Y1 per pixel pair
    PPA_MOD_YUV420 = 6,     // the P4's packed 4:2:0: U Y Y on the first line of each pair, V Y Y on the second
};

// Bytes for a w x h picture in a format (YUV420: 12 bits a pixel).
size_t ppa_mod_size(int fmt, uint32_t w, uint32_t h);

// One scale-rotate-mirror (and colour convert): the whole source picture, or
// its block (sx, sy, sw, sh; sw == 0 means all of it), scaled to fill the
// destination block (x, y, w, h; w == 0 means all of it) of the destination
// picture, after rotating by rotate (0, 90, 180 or 270 degrees, counter-
// clockwise, as the PPA turns) and mirroring.
typedef struct {
    const void *src;
    uint32_t src_w, src_h;
    int src_fmt;
    uint32_t sx, sy, sw, sh;
    void *dst;
    size_t dst_len;
    uint32_t dst_w, dst_h;
    int dst_fmt;
    uint32_t x, y, w, h;
    int rotate;
    bool mirror_x, mirror_y;
    bool swap;              // RGB565 source is big-endian
    bool yuv_limited;       // YUV is studio range (16..235) rather than full
    bool approx;            // the PPA may scale by its nearest sixteenth, filling a
                            // little less of the block, rather than leave it to software
} ppa_mod_srm_t;

typedef struct ppa_mod_client ppa_mod_client_t;

// A client, or NULL when there is no memory. hw: -1 the PPA where there is
// one, 0 software, 1 the PPA or fail.
ppa_mod_client_t *ppa_mod_open(int hw);
void ppa_mod_close(ppa_mod_client_t *c);

// NULL when done, else why not.
const char *ppa_mod_srm(ppa_mod_client_t *c, const ppa_mod_srm_t *op);

// Whether the last operation on c ran on the PPA.
bool ppa_mod_used_hw(ppa_mod_client_t *c);

#endif
