// tsmux writes exactly the packets castif's own muxer did.
//
// castif_ref.inc is castif's muxer verbatim from git (extract_castif_ref.py);
// this builds it beside tsmux_core.c, feeds both the same access units, LPCM
// blocks and PCR-only ticks, with and without the audio track, and compares
// every byte. `--plant` flips one byte of tsmux's output first, to show the
// comparison can fail.
//
//   cc -O2 -I../src -o compare_castif compare_castif.c && ./compare_castif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- castif, as it was ------------------------------------------------------
#define PID_PMT 0x1000
#define PID_VIDEO 0x100
#define PCR_LEAD 36000
#define A_BLOCK 1920

typedef struct {
    int audio_on;
    uint8_t cc_pat, cc_pmt, cc_video, cc_audio;
    uint8_t pat[188];
    uint8_t pmt[188];
    uint32_t pmt_len;
    uint8_t pkt[188];
    uint8_t pes_head[20];
    uint8_t apes[18];
} castif_obj_t;

static uint8_t *ref_buf;
static size_t ref_len;

static void rtp_push(castif_obj_t *c, const uint8_t *pkt) {
    (void)c;
    memcpy(ref_buf + ref_len, pkt, 188);
    ref_len += 188;
}

// the reference keeps its own names out of tsmux_core.c's way
#define crc32_mpeg ref_crc32_mpeg
#define build_section ref_build_section
#define build_tables ref_build_tables
#define ts_header ref_ts_header
#define pts_field ref_pts_field
#include "castif_ref.inc"
#undef crc32_mpeg
#undef build_section
#undef build_tables
#undef ts_header
#undef pts_field

// --- tsmux ------------------------------------------------------------------
#include "tsmux_core.c"

static uint8_t *new_buf;
static size_t new_len;

static void collect(void *ctx, const uint8_t *pkt) {
    (void)ctx;
    memcpy(new_buf + new_len, pkt, 188);
    new_len += 188;
}

// --- the same input to both ---------------------------------------------------
static uint32_t rng = 12345;
static uint32_t rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static int run(int audio, int plant) {
    castif_obj_t c;
    memset(&c, 0, sizeof(c));
    c.audio_on = audio;
    static const uint8_t H[20] = {0, 0, 1, 0xe0, 0, 0, 0x80, 0x80, 5, 0, 0, 0, 0, 0, 0, 0, 0, 1, 9, 0xf0};
    memcpy(c.pes_head, H, 20);
    ref_build_tables(&c);
    tsmux_t m;
    tsmux_init(&m, audio, collect, NULL);
    ref_len = new_len = 0;

    // sizes around every packet boundary the muxer cares about, then random ones
    static const uint32_t edges[] = {1, 2, 155, 156, 157, 163, 164, 165, 175, 176, 177, 182, 183, 184,
        185, 340, 341, 348, 360, 361, 368, 369, 1000, 65536, 200000};
    uint8_t *au = malloc(200000);
    uint8_t pcm[A_BLOCK];
    uint32_t pts = 90000;
    int frames = 0;
    for (int i = 0; i < 400; i++, pts += 3000) {
        uint32_t len = i < (int)(sizeof(edges) / sizeof(edges[0])) ? edges[i] : 1 + rnd() % 40000;
        for (uint32_t k = 0; k < len; k++) {
            au[k] = (uint8_t)rnd();
        }
        int key = (i % 30) == 0;
        if (i % 7 == 3) {                           // a skipped tick
            mux_pcr_only(&c, pts - PCR_LEAD);
            tsmux_pcr(&m, pts - PCR_LEAD);
        } else {
            if (key) {
                emit_tables(&c);
                tsmux_tables(&m);
            }
            mux_video(&c, au, len, pts, key);
            tsmux_video(&m, au, len, pts, key);
            frames++;
        }
        if (audio) {
            for (int b = 0; b < 3; b++) {
                for (int k = 0; k < A_BLOCK; k++) {
                    pcm[k] = (uint8_t)rnd();
                }
                mux_lpcm(&c, pcm, pts + b * 900);
                tsmux_lpcm(&m, pcm, A_BLOCK, pts + b * 900);
            }
        }
    }
    free(au);
    if (plant) {
        new_buf[new_len / 2] ^= 0x01;
    }
    size_t diff = ref_len == new_len ? 0 : 1;
    size_t first = (size_t)-1;
    for (size_t k = 0; k < ref_len && k < new_len; k++) {
        if (ref_buf[k] != new_buf[k]) {
            diff++;
            if (first == (size_t)-1) {
                first = k;
            }
        }
    }
    printf("audio=%d: %d frames, castif %zu packets, tsmux %zu packets, %s",
        audio, frames, ref_len / 188, new_len / 188, diff ? "DIFFERENT" : "identical");
    if (diff && first != (size_t)-1) {
        printf(" (first at byte %zu, packet %zu)", first, first / 188);
    }
    printf("\n");
    return diff != 0;
}

int main(int argc, char **argv) {
    int plant = argc > 1 && strcmp(argv[1], "--plant") == 0;
    ref_buf = malloc(64 << 20);
    new_buf = malloc(64 << 20);
    int bad = run(0, plant) | run(1, plant);
    printf(bad ? "FAIL\n" : "PASS: tsmux writes castif's stream byte for byte\n");
    return bad;
}
