// tsmux: H.264 access units and LPCM blocks to MPEG-TS packets (tsmux_core.h).
//
// Moved out of castif (media modules roadmap, Phase 4) without changing a byte
// it sends: tests/compare_castif.c holds castif's muxer as it was and checks
// that this one writes the same packets from the same input.

#include <stdlib.h>
#include <string.h>

#include "tsmux_core.h"

static uint32_t crc32_mpeg(const uint8_t *data, uint32_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        c ^= (uint32_t)data[i] << 24;
        for (int k = 0; k < 8; k++) {
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        }
    }
    return c;
}

// table_id, 0xB000|length, ident, 0xC1, 0, 0, body..., CRC32. Returns section length.
static uint32_t build_section(uint8_t *dst, uint8_t table_id, uint16_t ident,
    const uint8_t *body, uint32_t body_len) {
    uint32_t length = 5 + body_len + 4;     // counts everything after the length field
    uint32_t n = 0;
    dst[n++] = table_id;
    dst[n++] = (0xB0 | ((length >> 8) & 0x0F));
    dst[n++] = length & 0xFF;
    dst[n++] = ident >> 8;
    dst[n++] = ident & 0xFF;
    dst[n++] = 0xC1;
    dst[n++] = 0;
    dst[n++] = 0;
    memcpy(dst + n, body, body_len);
    n += body_len;
    uint32_t crc = crc32_mpeg(dst, n);
    dst[n++] = crc >> 24;
    dst[n++] = (crc >> 16) & 0xFF;
    dst[n++] = (crc >> 8) & 0xFF;
    dst[n++] = crc & 0xFF;
    return n;
}

static void build_tables(tsmux_t *m) {
    // PAT: program 1 -> PMT PID
    uint8_t pat_body[4];
    pat_body[0] = 0;
    pat_body[1] = 1;                                        // program_number 1
    pat_body[2] = 0xE0 | (TSMUX_PID_PMT >> 8);
    pat_body[3] = TSMUX_PID_PMT & 0xFF;
    uint32_t pat_sec = build_section(m->pat + 5, 0x00, 1, pat_body, 4);
    m->pat[4] = 0;                                          // pointer field
    // PMT: PCR PID = video, one video stream (0x1B), and LPCM when asked for
    uint8_t pmt[9 + 9];
    uint32_t n = 0;
    pmt[n++] = 0xE0 | (TSMUX_PID_VIDEO >> 8);
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;                      // PCR PID
    pmt[n++] = 0xF0;
    pmt[n++] = 0x00;                                        // program_info_length 0
    pmt[n++] = 0x1B;                                        // stream_type H.264
    pmt[n++] = 0xE0 | (TSMUX_PID_VIDEO >> 8);
    pmt[n++] = TSMUX_PID_VIDEO & 0xFF;
    pmt[n++] = 0xF0;
    pmt[n++] = 0x00;                                        // ES_info_length 0
    if (m->lpcm) {
        pmt[n++] = 0x83;                                    // HDMV LPCM
        pmt[n++] = 0xE0 | (TSMUX_PID_AUDIO >> 8);
        pmt[n++] = TSMUX_PID_AUDIO & 0xFF;
        pmt[n++] = 0xF0;
        pmt[n++] = 0x04;                                    // ES_info_length 4
        pmt[n++] = 0x83;                                    // registration descriptor...
        pmt[n++] = 0x02;
        pmt[n++] = 0x46;
        pmt[n++] = 0x2f;
    }
    uint32_t pmt_sec = build_section(m->pmt + 5, 0x02, 1, pmt, n);
    m->pmt[4] = 0;
    for (uint32_t i = 5 + pat_sec; i < 188; i++) {
        m->pat[i] = 0xFF;
    }
    for (uint32_t i = 5 + pmt_sec; i < 188; i++) {
        m->pmt[i] = 0xFF;
    }
}

static void ts_header(uint8_t *pkt, uint16_t pid, uint8_t *cc, int pusi, int afc) {
    pkt[0] = 0x47;
    pkt[1] = (pusi ? 0x40 : 0) | (pid >> 8);
    pkt[2] = pid & 0xFF;
    pkt[3] = (afc << 4) | (*cc & 15);
    *cc = (*cc + 1) & 15;
}

static void pts_field(uint8_t *buf, int off, uint32_t pts) {
    buf[off] = 0x21 | ((pts >> 29) & 0x0E);
    buf[off + 1] = (pts >> 22) & 0xFF;
    buf[off + 2] = 0x01 | ((pts >> 14) & 0xFE);
    buf[off + 3] = (pts >> 7) & 0xFF;
    buf[off + 4] = 0x01 | ((pts << 1) & 0xFE);
}

void tsmux_init(tsmux_t *m, int lpcm, tsmux_out_fn *out, void *ctx) {
    memset(m, 0, sizeof(*m));
    m->out = out;
    m->ctx = ctx;
    m->lpcm = lpcm;
    // PES header (stream 0xE0, unbounded, PTS only) + access-unit delimiter
    static const uint8_t H[20] = {0, 0, 1, 0xe0, 0, 0, 0x80, 0x80, 5, 0, 0, 0, 0, 0, 0, 0, 0, 1, 9, 0xf0};
    memcpy(m->pes_head, H, 20);
    build_tables(m);
}

tsmux_t *tsmux_new(int lpcm, tsmux_out_fn *out, void *ctx) {
    tsmux_t *m = malloc(sizeof(tsmux_t));
    if (m != NULL) {
        tsmux_init(m, lpcm, out, ctx);
    }
    return m;
}

void tsmux_free(tsmux_t *m) {
    free(m);
}

void tsmux_reset(tsmux_t *m) {
    m->cc_pat = m->cc_pmt = m->cc_video = m->cc_audio = 0;
}

void tsmux_tables(tsmux_t *m) {
    m->pat[3] = (1 << 4) | (m->cc_pat & 15);
    m->cc_pat = (m->cc_pat + 1) & 15;
    m->pat[0] = 0x47;
    m->pat[1] = 0x40;
    m->pat[2] = 0x00;
    m->out(m->ctx, m->pat);
    m->pmt[3] = (1 << 4) | (m->cc_pmt & 15);
    m->cc_pmt = (m->cc_pmt + 1) & 15;
    m->pmt[0] = 0x47;
    m->pmt[1] = 0x40 | (TSMUX_PID_PMT >> 8);
    m->pmt[2] = TSMUX_PID_PMT & 0xFF;
    m->out(m->ctx, m->pmt);
}

void tsmux_video(tsmux_t *m, const uint8_t *au, uint32_t au_len, uint32_t pts, int key) {
    uint8_t *pkt = m->pkt;
    uint8_t *head = m->pes_head;
    pts_field(head, 9, pts);
    uint32_t hlen = 20;
    uint32_t total = hlen + au_len;
    uint32_t pos = 0;
    uint32_t pcr = pts - TSMUX_PCR_LEAD;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184;
        uint32_t body = 4;
        if (first) {
            ts_header(pkt, TSMUX_PID_VIDEO, &m->cc_video, 1, 3);
            pkt[4] = 7;
            pkt[5] = key ? 0x50 : 0x10;     // random_access_indicator + PCR flag
            pkt[6] = (pcr >> 25) & 0xFF;
            pkt[7] = (pcr >> 17) & 0xFF;
            pkt[8] = (pcr >> 9) & 0xFF;
            pkt[9] = (pcr >> 1) & 0xFF;
            pkt[10] = ((pcr & 1) << 7) | 0x7E;
            pkt[11] = 0;
            body = 12;
            room = 176;
        }
        uint32_t remaining = total - pos;
        if (remaining < room) {
            uint32_t pad = room - remaining;
            if (!first) {
                ts_header(pkt, TSMUX_PID_VIDEO, &m->cc_video, 0, 3);
                pkt[4] = pad - 1;
                if (pad >= 2) {
                    pkt[5] = 0;
                    for (uint32_t i = 6; i < 4 + pad; i++) {
                        pkt[i] = 0xFF;
                    }
                }
                body = 4 + pad;
            } else {
                // extend the PCR adaptation field
                pkt[4] = 7 + pad;
                for (uint32_t i = 12; i < 12 + pad; i++) {
                    pkt[i] = 0xFF;
                }
                body = 12 + pad;
            }
            room = remaining;
        } else if (!first) {
            ts_header(pkt, TSMUX_PID_VIDEO, &m->cc_video, 0, 1);
        }
        uint32_t n = room;
        uint32_t dst = body;
        if (pos < hlen) {
            uint32_t hh = (hlen - pos < n) ? (hlen - pos) : n;
            memcpy(pkt + dst, head + pos, hh);
            dst += hh;
            pos += hh;
            n -= hh;
        }
        if (n) {
            memcpy(pkt + dst, au + (pos - hlen), n);
            pos += n;
        }
        m->out(m->ctx, pkt);
        first = 0;
    }
}

void tsmux_lpcm(tsmux_t *m, const uint8_t *pcm, uint32_t len, uint32_t pts) {
    uint8_t *pkt = m->pkt;
    uint8_t *head = m->apes;
    uint32_t n = len + 4;
    head[0] = 0;
    head[1] = 0;
    head[2] = 1;
    head[3] = 0xbd;                                     // private stream 1
    head[4] = ((n + 8) >> 8) & 0xFF;
    head[5] = (n + 8) & 0xFF;
    head[6] = 0x80;
    head[7] = 0x80;
    head[8] = 0x05;
    pts_field(head, 9, pts);
    head[14] = 0xA0;                                    // LPCM audio header:
    head[15] = 6;                                       // 48 kHz, 16-bit, stereo
    head[16] = 0;
    head[17] = 0x11;
    uint32_t hlen = 18, total = hlen + len, pos = 0;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184, body = 4;
        uint32_t remaining = total - pos;
        if (remaining < room) {
            uint32_t pad = room - remaining;
            ts_header(pkt, TSMUX_PID_AUDIO, &m->cc_audio, first, 3);
            pkt[4] = pad - 1;
            if (pad >= 2) {
                pkt[5] = 0;
                for (uint32_t i = 6; i < 4 + pad; i++) {
                    pkt[i] = 0xFF;
                }
            }
            body = 4 + pad;
            room = remaining;
        } else {
            ts_header(pkt, TSMUX_PID_AUDIO, &m->cc_audio, first, 1);
        }
        uint32_t k = room, dst = body;
        if (pos < hlen) {
            uint32_t hh = (hlen - pos < k) ? (hlen - pos) : k;
            memcpy(pkt + dst, head + pos, hh);
            dst += hh;
            pos += hh;
            k -= hh;
        }
        if (k) {
            // native little-endian in, big-endian on the wire: the 18-byte
            // header and 184-byte payloads keep the offsets even, so samples
            // stay whole
            uint32_t off = pos - hlen;
            for (uint32_t i = 0; i < k; i++) {
                pkt[dst + i] = pcm[(off + i) ^ 1];
            }
            pos += k;
        }
        m->out(m->ctx, pkt);
        first = 0;
    }
}

void tsmux_pcr(tsmux_t *m, uint32_t pcr) {
    uint8_t *pkt = m->pkt;
    pkt[0] = 0x47;
    pkt[1] = TSMUX_PID_VIDEO >> 8;
    pkt[2] = TSMUX_PID_VIDEO & 0xFF;
    pkt[3] = (2 << 4) | ((m->cc_video - 1) & 15);   // adaptation field only: cc does not advance
    pkt[4] = 183;
    pkt[5] = 0x10;
    pkt[6] = (pcr >> 25) & 0xFF;
    pkt[7] = (pcr >> 17) & 0xFF;
    pkt[8] = (pcr >> 9) & 0xFF;
    pkt[9] = (pcr >> 1) & 0xFF;
    pkt[10] = ((pcr & 1) << 7) | 0x7E;
    pkt[11] = 0;
    memset(pkt + 12, 0xFF, 176);
    m->out(m->ctx, pkt);
}
