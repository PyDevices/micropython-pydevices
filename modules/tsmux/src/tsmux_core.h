// tsmux: H.264 access units and LPCM blocks to MPEG-TS packets. Portable C
// with no MicroPython in it: castif's cast task links it on core 0, and
// mod_tsmux.c wraps it for Python on every port.
//
// The stream is the one castif has always sent, which the Windows Wireless
// Display receiver and Roku OS accept: program 1, PMT on PID 0x1000, H.264 on
// PID 0x100 (also the PCR PID), LPCM on PID 0x101 as HDMV stream type 0x83
// with its registration descriptor. Every packet goes to an output callback.
//
// A module that may be built without tsmux declares these weak (castif does).
#ifndef TSMUX_CORE_H
#define TSMUX_CORE_H

#include <stdint.h>

#define TSMUX_PID_PMT 0x1000
#define TSMUX_PID_VIDEO 0x100
#define TSMUX_PID_AUDIO 0x101
#define TSMUX_PCR_LEAD 36000        // the PCR runs 400 ms (90 kHz) ahead of the PTS

// Receives each 188-byte TS packet, in order.
typedef void tsmux_out_fn(void *ctx, const uint8_t *pkt);

typedef struct tsmux {
    tsmux_out_fn *out;
    void *ctx;
    int lpcm;                       // the PMT lists an LPCM track
    uint8_t cc_pat, cc_pmt, cc_video, cc_audio;
    uint8_t pat[188];
    uint8_t pmt[188];
    uint8_t pkt[188];
    uint8_t pes_head[20];           // video PES header + access-unit delimiter
    uint8_t apes[18];               // LPCM PES header
} tsmux_t;

// Sets up m: tables built, continuity counters at 0. lpcm adds the audio track.
void tsmux_init(tsmux_t *m, int lpcm, tsmux_out_fn *out, void *ctx);

// A heap-allocated muxer, for code that holds it by pointer without this
// header (castif): NULL when out of memory. tsmux_free(NULL) is fine.
tsmux_t *tsmux_new(int lpcm, tsmux_out_fn *out, void *ctx);
void tsmux_free(tsmux_t *m);

// Continuity counters back to 0, as for a new stream.
void tsmux_reset(tsmux_t *m);

// The PAT and the PMT (send them before each keyframe).
void tsmux_tables(tsmux_t *m);

// One H.264 access unit (Annex B) at pts (90 kHz), with an access-unit
// delimiter in front and the PCR (pts - TSMUX_PCR_LEAD) in its first packet;
// key marks a random access point.
void tsmux_video(tsmux_t *m, const uint8_t *au, uint32_t len, uint32_t pts, int key);

// One block of 48 kHz stereo s16 PCM, native little-endian (sent big-endian,
// as LPCM is), at pts. len is a multiple of 4.
void tsmux_lpcm(tsmux_t *m, const uint8_t *pcm, uint32_t len, uint32_t pts);

// A PCR alone on the video PID, for a tick with no frame: the sink's clock
// keeps running while the picture is still.
void tsmux_pcr(tsmux_t *m, uint32_t pcr);

#endif
