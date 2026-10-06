// What castif calls in h264enc and tsmux, declared rather than included,
// and weak, so castif builds and links without either in the firmware; Cast()
// and Hls() then say what is missing. Shared by mod_castif.c and castif_hls.c.
#ifndef CASTIF_EXT_H
#define CASTIF_EXT_H

#include <stdbool.h>
#include <stdint.h>

// h264enc encodes (modules/h264enc/src/h264enc.h).
typedef struct h264enc_session h264enc_session_t;
extern const char *h264enc_open(h264enc_session_t **out, int width, int height, int canvas_w, int canvas_h,
    int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size) __attribute__((weak));
extern int h264enc_encode(h264enc_session_t *s, const uint8_t *rgb565, const uint8_t **data, uint32_t *len, bool *idr) __attribute__((weak));
extern void h264enc_force_idr(h264enc_session_t *s) __attribute__((weak));
extern void h264enc_set_bitrate(h264enc_session_t *s, uint32_t bps) __attribute__((weak));
extern void h264enc_timing(h264enc_session_t *s, uint32_t *ppa_us, uint32_t *enc_us) __attribute__((weak));
extern void h264enc_close(h264enc_session_t *s) __attribute__((weak));

// tsmux writes the MPEG-TS (modules/tsmux/src/tsmux_core.h), held by pointer.
typedef struct tsmux tsmux_t;
typedef void tsmux_out_fn(void *ctx, const uint8_t *pkt);
extern tsmux_t *tsmux_new(int lpcm, tsmux_out_fn *out, void *ctx) __attribute__((weak));
extern void tsmux_free(tsmux_t *m) __attribute__((weak));
extern void tsmux_reset(tsmux_t *m) __attribute__((weak));
extern void tsmux_tables(tsmux_t *m) __attribute__((weak));
extern void tsmux_video(tsmux_t *m, const uint8_t *au, uint32_t len, uint32_t pts, int key) __attribute__((weak));
extern void tsmux_lpcm(tsmux_t *m, const uint8_t *pcm, uint32_t len, uint32_t pts) __attribute__((weak));
extern void tsmux_pcr(tsmux_t *m, uint32_t pcr) __attribute__((weak));

#endif
