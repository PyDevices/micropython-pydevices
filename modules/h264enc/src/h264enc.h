// h264enc's C API, for native code that encodes without Python in the loop
// (castif's core-0 cast task is the first user).
//
// A module that may be built without h264enc declares these weak and checks
// h264enc_open for NULL before using any of them, so it still links.
//
// The P4 has one H.264 encoder, so there is one session at a time:
// h264enc_open() returns "busy" while another is open (a Python Encoder or a
// cast), and the open one carries on untouched.
#ifndef H264ENC_H
#define H264ENC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct h264enc_session h264enc_session_t;

// Opens the encoder for width x height RGB565 frames (multiples of 16), placed
// centred on a canvas_w x canvas_h picture (0 = the frame's own size; the
// border is black). out_size caps one access unit (0 = 256 KB). Returns NULL
// and sets *out, or returns why not: "busy", or a short reason.
const char *h264enc_open(h264enc_session_t **out, int width, int height, int canvas_w, int canvas_h,
    int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size);

// Encodes one frame (width * height * 2 bytes of native RGB565). On success
// returns 0 and points *data at the access unit (Annex B NAL units, valid
// until the next call), with its length and whether it is an IDR frame.
int h264enc_encode(h264enc_session_t *s, const uint8_t *rgb565, const uint8_t **data, uint32_t *len, bool *idr);

// Next frame is an IDR; a new target bitrate from the next frame on. Call
// from the thread that encodes.
void h264enc_force_idr(h264enc_session_t *s);
void h264enc_set_bitrate(h264enc_session_t *s, uint32_t bps);

// Microseconds the last encode spent in the PPA conversion and the encoder.
void h264enc_timing(h264enc_session_t *s, uint32_t *ppa_us, uint32_t *enc_us);

// Frees everything and gives the encoder back. NULL is fine.
void h264enc_close(h264enc_session_t *s);

#endif
