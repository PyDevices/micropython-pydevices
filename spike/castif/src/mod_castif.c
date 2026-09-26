// castif (spike, Phase 1): the cast as a FreeRTOS task on core 0.
//
// The spike proved the path in Python (h264enc + tsmux.py + a pump loop). This
// moves the MPEG-TS mux and the RTP send into C and runs the whole loop --
// PPA convert, hardware H.264 encode, packetize, send -- on core 0, off the
// Python GIL. The app keeps its frame rate on core 1, and the receiver, fed a
// steady stream, foregrounds at once instead of after ~35 s.
//
// Phase 2 adds sound: audio=True gives the Cast a ring of 10 ms LPCM blocks that
// Python fills from the audio pump (feed_audio) and the task muxes on the same
// wall clock as the video.
//
//   c = castif.Cast(w, h, canvas_w=1280, canvas_h=720, fps=30, bitrate=3_000_000)
//   c.start(framebuffer, sink_ip, dst_port, src_port)   # after RTSP PLAY
//   c.stats(); c.force_idr(); c.set_bitrate(n); c.set_fps(n); c.stop()
//
// The MICE + RTSP session stays in Python (micecast.py); it is idle after PLAY.

#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objstr.h"

#ifndef NO_QSTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "driver/ppa.h"
#endif

#define PID_PMT   0x1000
#define PID_VIDEO 0x100
#define PCR_LEAD  36000           // 400 ms in 90 kHz ticks
#define AU_MAX    (256 * 1024)    // encoder output cap when out_size not given
#define A_BLOCK   1920            // one 10 ms LPCM block: 480 frames * 4 bytes (48 kHz stereo s16)
#define A_BLOCKS  64              // ring depth: 640 ms of audio

typedef struct _castif_obj_t {
    mp_obj_base_t base;
    // encoder + PPA (as h264enc)
    esp_h264_enc_handle_t enc;
    esp_h264_enc_param_hw_handle_t param;
    ppa_client_handle_t ppa;
    uint8_t *yuv;
    uint32_t yuv_len;
    uint32_t in_len;              // packed YUV420 canvas the encoder reads
    uint8_t *out_buf;
    uint32_t out_cap;
    uint16_t w, h, cw, ch;
    bool open;
    // source framebuffer (RGB565), pinned by start()
    mp_obj_t fb_obj;
    const uint8_t *fb;
    uint32_t fb_len;
    // TS mux
    uint8_t cc_pat, cc_pmt, cc_video;
    uint8_t pat[188];
    uint8_t pmt[188];
    uint32_t pmt_len;
    uint8_t pkt[188];
    uint8_t pes_head[20];         // video PES header + access-unit delimiter
    // RTP
    int sock;
    struct sockaddr_in dst;
    uint16_t seq;
    uint32_t ssrc;
    uint32_t ts90;
    uint8_t rtp_buf[12 + 7 * 188];
    int rtp_n;
    // task + control
    TaskHandle_t task;
    volatile bool running;
    volatile bool want_idr;
    volatile bool dirty;   // a real change signal (mark_dirty): beats the sampled hash
    volatile int new_bitrate;
    volatile uint32_t frame_us;
    // skip-unchanged (lever a): a cheap sampled hash of the framebuffer; when
    // it has not changed and a real frame went out recently, skip the PPA +
    // encode + send so a static UI costs almost no memory bandwidth.
    uint32_t last_hash;
    int64_t last_real_us;
    uint32_t max_skip_us;        // send at least this often even when static (0 = feature off)
    // stats
    volatile uint32_t frames, packets, sent, stalls, idr_reqs, skipped;
    // audio (Phase 2): Python feeds 10 ms LPCM blocks into this ring; the core-0
    // task drains it and muxes on the shared 90 kHz clock (audio kept ~100 ms
    // ahead of the video, as the Python spike did).
    bool audio_on;
    uint8_t *audio_ring;                 // A_BLOCKS * A_BLOCK, in SPIRAM
    volatile uint32_t a_head, a_tail;    // block write / read indices
    uint8_t cc_audio;
    uint8_t apes[18];
    uint32_t apts;                        // audio PTS, 90 kHz
    int64_t t0;                           // esp_timer at start(): the wall clock both PTS come from
    uint32_t a_stamp[A_BLOCKS];           // wall-clock (90 kHz) when each block was fed
    int32_t drift, drift_ref;             // EMA of stamp - apts, and its value once settled
    bool drift_ref_set;
    uint32_t sync_limit;                  // drop/insert past this (90 kHz ticks); 0 = measure only
    volatile uint32_t audio_fed, audio_muxed, audio_underruns, audio_inserted, audio_dropped;
    volatile uint32_t enc_us, ppa_us, mux_us, send_us;
    volatile uint32_t last_len;
    volatile int last_type;
    volatile uint32_t mfps;       // measured fps * 1000
} castif_obj_t;

// ---- MPEG-TS section building (PAT/PMT) ----------------------------------

static uint32_t crc32_mpeg(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
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
    uint32_t length = 5 + body_len + 4;   // counts everything after the length field
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

static void build_tables(castif_obj_t *c) {
    // PAT: program 1 -> PMT PID
    uint8_t pat_body[4];
    pat_body[0] = 0; pat_body[1] = 1;                       // program_number 1
    pat_body[2] = 0xE0 | (PID_PMT >> 8); pat_body[3] = PID_PMT & 0xFF;
    uint32_t pat_sec = build_section(c->pat + 5, 0x00, 1, pat_body, 4);
    c->pat[4] = 0;                                          // pointer field
    // PMT: PCR PID = video, one video stream (0x1B)
    uint8_t pmt_body[9];
    pmt_body[0] = 0xE0 | (PID_VIDEO >> 8); pmt_body[1] = PID_VIDEO & 0xFF;  // PCR PID
    pmt_body[2] = 0xF0; pmt_body[3] = 0x00;                 // program_info_length 0
    pmt_body[4] = 0x1B;                                     // stream_type H.264
    pmt_body[5] = 0xE0 | (PID_VIDEO >> 8); pmt_body[6] = PID_VIDEO & 0xFF;
    pmt_body[7] = 0xF0; pmt_body[8] = 0x00;                 // ES_info_length 0
    uint8_t pmt_a[9 + 5 + 6];
    memcpy(pmt_a, pmt_body, 9);
    uint32_t pmt_n = 9;
    if (c->audio_on) {
        pmt_a[pmt_n++] = 0x83;                             // HDMV LPCM
        pmt_a[pmt_n++] = 0xE0 | (0x101 >> 8); pmt_a[pmt_n++] = 0x101 & 0xFF;
        pmt_a[pmt_n++] = 0xF0; pmt_a[pmt_n++] = 0x04;      // ES_info_length 4
        pmt_a[pmt_n++] = 0x83; pmt_a[pmt_n++] = 0x02; pmt_a[pmt_n++] = 0x46; pmt_a[pmt_n++] = 0x2f;
    }
    c->pmt_len = build_section(c->pmt + 5, 0x02, 1, pmt_a, pmt_n);
    c->pmt[4] = 0;
    // pad both to 188
    for (uint32_t i = 5 + pat_sec; i < 188; i++) c->pat[i] = 0xFF;
    for (uint32_t i = 5 + c->pmt_len; i < 188; i++) c->pmt[i] = 0xFF;
}

// ---- TS packet header ----------------------------------------------------

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

// ---- RTP (7 TS packets per RTP, payload type 33) -------------------------

static void rtp_flush(castif_obj_t *c) {
    if (!c->rtp_n) return;
    uint8_t *b = c->rtp_buf;
    b[0] = 0x80; b[1] = 33;
    b[2] = (c->seq >> 8) & 0xFF; b[3] = c->seq & 0xFF;
    b[4] = (c->ts90 >> 24) & 0xFF; b[5] = (c->ts90 >> 16) & 0xFF;
    b[6] = (c->ts90 >> 8) & 0xFF; b[7] = c->ts90 & 0xFF;
    b[8] = (c->ssrc >> 24) & 0xFF; b[9] = (c->ssrc >> 16) & 0xFF;
    b[10] = (c->ssrc >> 8) & 0xFF; b[11] = c->ssrc & 0xFF;
    int end = 12 + c->rtp_n * 188;
    for (;;) {
        int r = sendto(c->sock, b, end, 0, (struct sockaddr *)&c->dst, sizeof(c->dst));
        if (r >= 0) break;
        c->stalls++;
        if (c->stalls > 200000) break;
        vTaskDelay(1);
    }
    c->seq++;
    c->sent++;
    c->rtp_n = 0;
}

static void rtp_push(castif_obj_t *c, const uint8_t *pkt) {
    memcpy(c->rtp_buf + 12 + c->rtp_n * 188, pkt, 188);
    c->rtp_n++;
    c->packets++;
    if (c->rtp_n == 7) rtp_flush(c);
}

// ---- video PES -> TS packets (port of tsmux._pes for the video PID) -------

static void mux_video(castif_obj_t *c, const uint8_t *au, uint32_t au_len,
                      uint32_t pts, int key) {
    uint8_t *pkt = c->pkt;
    uint8_t *head = c->pes_head;        // 20 bytes: PES header + AUD
    pts_field(head, 9, pts);
    uint32_t hlen = 20;
    uint32_t total = hlen + au_len;
    uint32_t pos = 0;
    uint32_t pcr = pts - PCR_LEAD;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184;
        uint32_t body = 4;
        if (first) {
            ts_header(pkt, PID_VIDEO, &c->cc_video, 1, 3);
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
                ts_header(pkt, PID_VIDEO, &c->cc_video, 0, 3);
                pkt[4] = pad - 1;
                if (pad >= 2) {
                    pkt[5] = 0;
                    for (uint32_t i = 6; i < 4 + pad; i++) pkt[i] = 0xFF;
                }
                body = 4 + pad;
            } else {
                // extend the PCR adaptation field
                pkt[4] = 7 + pad;
                for (uint32_t i = 12; i < 12 + pad; i++) pkt[i] = 0xFF;
                body = 12 + pad;
            }
            room = remaining;
        } else if (!first) {
            ts_header(pkt, PID_VIDEO, &c->cc_video, 0, 1);
        }
        uint32_t n = room;
        uint32_t dst = body;
        if (pos < hlen) {
            uint32_t hh = (hlen - pos < n) ? (hlen - pos) : n;
            memcpy(pkt + dst, head + pos, hh);
            dst += hh; pos += hh; n -= hh;
        }
        if (n) {
            memcpy(pkt + dst, au + (pos - hlen), n);
            pos += n;
        }
        rtp_push(c, pkt);
        first = 0;
    }
}

static void mux_lpcm(castif_obj_t *c, const uint8_t *pcm, uint32_t pts) {
    uint8_t *pkt = c->pkt;
    uint8_t *head = c->apes;               // 18 bytes
    uint32_t n = A_BLOCK + 4;
    head[0]=0;head[1]=0;head[2]=1;head[3]=0xbd;
    head[4]=((n+8)>>8)&0xFF; head[5]=(n+8)&0xFF;
    head[6]=0x80; head[7]=0x80; head[8]=0x05;
    pts_field(head, 9, pts);
    head[14]=0xA0; head[15]=6; head[16]=0; head[17]=0x11;
    uint32_t hlen = 18, total = hlen + A_BLOCK, pos = 0;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184, body = 4;
        uint32_t remaining = total - pos;
        if (remaining < room) {
            uint32_t pad = room - remaining;
            ts_header(pkt, 0x101, &c->cc_audio, first, 3);
            pkt[4] = pad - 1;
            if (pad >= 2) { pkt[5] = 0; for (uint32_t i = 6; i < 4 + pad; i++) pkt[i] = 0xFF; }
            body = 4 + pad; room = remaining;
        } else {
            ts_header(pkt, 0x101, &c->cc_audio, first, 1);
        }
        uint32_t k = room, dst = body;
        if (pos < hlen) { uint32_t hh = (hlen - pos < k) ? (hlen - pos) : k; memcpy(pkt+dst, head+pos, hh); dst+=hh; pos+=hh; k-=hh; }
        if (k) { memcpy(pkt+dst, pcm + (pos - hlen), k); pos += k; }
        rtp_push(c, pkt);
        first = 0;
    }
}

static void emit_tables(castif_obj_t *c) {
    c->pat[3] = (1 << 4) | (c->cc_pat & 15); c->cc_pat = (c->cc_pat + 1) & 15;
    c->pat[0] = 0x47; c->pat[1] = 0x40; c->pat[2] = 0x00;
    rtp_push(c, c->pat);
    c->pmt[3] = (1 << 4) | (c->cc_pmt & 15); c->cc_pmt = (c->cc_pmt + 1) & 15;
    c->pmt[0] = 0x47; c->pmt[1] = 0x40 | (PID_PMT >> 8); c->pmt[2] = PID_PMT & 0xFF;
    rtp_push(c, c->pmt);
}

// ---- the streaming task (core 0) -----------------------------------------

static int ppa_convert(castif_obj_t *c) {
    int64_t p0 = esp_timer_get_time();
    ppa_srm_oper_config_t op = {0};
    op.in.buffer = (void *)c->fb;
    op.in.pic_w = c->w; op.in.pic_h = c->h;
    op.in.block_w = c->w; op.in.block_h = c->h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    op.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = c->yuv;
    op.out.buffer_size = c->yuv_len;
    op.out.pic_w = c->cw; op.out.pic_h = c->ch;
    op.out.block_offset_x = ((c->cw - c->w) / 2) & ~1u;
    op.out.block_offset_y = ((c->ch - c->h) / 2) & ~1u;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    op.out.yuv_range = PPA_COLOR_RANGE_LIMIT;
    op.out.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = 1.0f; op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t err = ppa_do_scale_rotate_mirror(c->ppa, &op);
    c->ppa_us = (uint32_t)(esp_timer_get_time() - p0);
    return err == ESP_OK ? 0 : -1;
}

static void mux_pcr_only(castif_obj_t *c, uint32_t pcr) {
    // a PCR with no payload on the video PID, for ticks whose frame was skipped:
    // the sink's clock keeps running while the picture is static and audio plays
    uint8_t *pkt = c->pkt;
    pkt[0] = 0x47; pkt[1] = PID_VIDEO >> 8; pkt[2] = PID_VIDEO & 0xFF;
    pkt[3] = (2 << 4) | ((c->cc_video - 1) & 15);   // adaptation field only: cc does not advance
    pkt[4] = 183;
    pkt[5] = 0x10;
    pkt[6] = (pcr >> 25) & 0xFF; pkt[7] = (pcr >> 17) & 0xFF;
    pkt[8] = (pcr >> 9) & 0xFF; pkt[9] = (pcr >> 1) & 0xFF;
    pkt[10] = ((pcr & 1) << 7) | 0x7E; pkt[11] = 0;
    memset(pkt + 12, 0xFF, 176);
    rtp_push(c, pkt);
}

// ---- audio (Phase 2) ------------------------------------------------------
//
// Python feeds 10 ms blocks as the pump plays them; feed_audio stamps each with
// the wall clock. Every tick, skipped frames included, the task sends all that
// is waiting: the first block takes the tick's PTS and the rest follow
// contiguously, so the sink hears one unbroken LPCM track on the video's clock.
// The stamps measure how far that track drifts from the wall clock (the pump's
// I2S rate against esp_timer). Past sync_limit the task drops one block or
// inserts one silent one -- 10 ms at a time, never a PTS jump.

static const uint8_t SILENCE[A_BLOCK] = {0};

static void drain_audio(castif_obj_t *c, uint32_t pts) {
    while (c->a_tail != c->a_head) {
        uint32_t i = c->a_tail % A_BLOCKS;
        if (c->apts == 0) { c->apts = pts; c->drift = 0; c->drift_ref_set = false; }
        int32_t d = (int32_t)(c->a_stamp[i] - c->apts);   // + : the track sits behind the clock
        c->drift += (d - c->drift) / 64;
        if (!c->drift_ref_set) {
            if (c->audio_muxed >= 300) { c->drift_ref = c->drift; c->drift_ref_set = true; }
        } else if (c->sync_limit) {
            int32_t off = c->drift - c->drift_ref;
            if (off > (int32_t)c->sync_limit) {          // audio would play early: pad
                mux_lpcm(c, SILENCE, c->apts);
                c->apts += 900; c->audio_inserted++; c->drift -= 900;
                continue;
            }
            if (off < -(int32_t)c->sync_limit) {         // audio would play late: drop one
                c->a_tail++; c->audio_dropped++; c->drift += 900;
                continue;
            }
        }
        mux_lpcm(c, c->audio_ring + i * A_BLOCK, c->apts);
        c->a_tail++; c->apts += 900; c->audio_muxed++;
    }
    // the producer has stopped: the track is more than 100 ms behind the picture
    if (c->apts && (int32_t)(pts - c->apts) > 9000) c->audio_underruns++;
}

// ---- the streaming task (core 0) -----------------------------------------

static void cast_task(void *arg) {
    castif_obj_t *c = (castif_obj_t *)arg;
    int64_t next = esp_timer_get_time();
    int64_t win_t0 = next;
    uint32_t win_frames = 0;
    while (c->running) {
        int64_t now = esp_timer_get_time();
        if (now < next) { vTaskDelay(1); continue; }
        next += c->frame_us;
        if (now - next > 200000) next = now;        // don't chase a big backlog
        // controls
        if (c->new_bitrate > 0) {
            esp_h264_enc_set_bitrate((esp_h264_enc_param_handle_t)c->param, c->new_bitrate);
            c->new_bitrate = -1;
        }
        int idr = c->want_idr;
        if (idr) {
            esp_h264_enc_force_idr((esp_h264_enc_param_handle_t)c->param);
            c->want_idr = false;
        }
        // one wall clock for both tracks: this tick's PTS in 90 kHz ticks
        uint32_t pts = 90000 + (uint32_t)(((now - c->t0) * 9) / 100);
        int skip = 0;
        if (c->max_skip_us) {
            const uint32_t *w = (const uint32_t *)c->fb;
            uint32_t words = c->fb_len / 4;
            uint32_t hsh = 2166136261u;
            for (uint32_t i = 0; i < words; i += 512) hsh = (hsh ^ w[i]) * 16777619u;
            if (hsh == c->last_hash && !idr && !c->dirty && (now - c->last_real_us) < (int64_t)c->max_skip_us) {
                c->skipped++;
                skip = 1;
            } else {
                c->last_hash = hsh;
                c->last_real_us = now;
                c->dirty = false;
            }
        }
        int64_t m0 = now;
        c->ts90 = pts - PCR_LEAD;
        if (!skip && ppa_convert(c) != 0) skip = 1;
        if (!skip) {
            esp_h264_enc_in_frame_t inf = {0};
            inf.raw_data.buffer = c->yuv;
            inf.raw_data.len = c->in_len;
            inf.pts = (uint32_t)((uint64_t)c->frames * c->frame_us / 1000);
            esp_h264_enc_out_frame_t outf = {0};
            outf.raw_data.buffer = c->out_buf;
            outf.raw_data.len = c->out_cap;
            int64_t e0 = esp_timer_get_time();
            esp_h264_err_t err = esp_h264_enc_process(c->enc, &inf, &outf);
            c->enc_us = (uint32_t)(esp_timer_get_time() - e0);
            if (err != ESP_H264_ERR_OK) {
                skip = 1;
            } else {
                c->last_len = outf.length;
                c->last_type = outf.frame_type;
                int key = (outf.frame_type == 0);
                m0 = esp_timer_get_time();
                if (key) emit_tables(c);
                mux_video(c, c->out_buf, outf.length, pts, key);
                c->frames++;
                win_frames++;
            }
        }
        if (c->audio_on) {
            if (skip) mux_pcr_only(c, pts - PCR_LEAD);
            drain_audio(c, pts);
        }
        rtp_flush(c);
        if (!skip) c->mux_us = (uint32_t)(esp_timer_get_time() - m0);
        if (now - win_t0 >= 1000000) {
            c->mfps = (uint32_t)((uint64_t)win_frames * 1000000000ull / (now - win_t0));
            win_t0 = now; win_frames = 0;
        }
    }
    if (c->sock >= 0) { closesocket(c->sock); c->sock = -1; }
    c->task = NULL;
    vTaskDelete(NULL);
}

// ---- Python API ----------------------------------------------------------

static void castif_check(esp_h264_err_t err, const char *what) {
    if (err != ESP_H264_ERR_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s: esp_h264 error %d"), what, (int)err);
    }
}

static mp_obj_t castif_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_canvas_w, ARG_canvas_h, ARG_fps, ARG_gop, ARG_bitrate, ARG_qp_min, ARG_qp_max, ARG_out_size, ARG_audio };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_canvas_w, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_canvas_h, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fps, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_gop, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 30} },
        { MP_QSTR_bitrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 3000000} },
        { MP_QSTR_qp_min, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10} },
        { MP_QSTR_qp_max, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 45} },
        { MP_QSTR_out_size, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_audio, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    int w = args[ARG_width].u_int, h = args[ARG_height].u_int;
    if (w < 80 || w > 1920 || h < 80 || h > 2032 || (w & 15) || (h & 15)) {
        mp_raise_ValueError(MP_ERROR_TEXT("width/height multiples of 16, 80..1920 x 80..2032"));
    }
    int cw = args[ARG_canvas_w].u_int ? args[ARG_canvas_w].u_int : w;
    int ch = args[ARG_canvas_h].u_int ? args[ARG_canvas_h].u_int : h;
    if (cw < w || ch < h || (cw & 15) || (ch & 15) || cw > 1920 || ch > 2032) {
        mp_raise_ValueError(MP_ERROR_TEXT("canvas multiple of 16 and at least the picture"));
    }
    castif_obj_t *self = mp_obj_malloc_with_finaliser(castif_obj_t, type);
    memset((char *)self + sizeof(mp_obj_base_t), 0, sizeof(castif_obj_t) - sizeof(mp_obj_base_t));
    self->sock = -1;
    self->w = w; self->h = h; self->cw = cw; self->ch = ch;
    self->new_bitrate = -1;
    self->frame_us = 1000000 / args[ARG_fps].u_int;
    self->in_len = (uint32_t)cw * ch * 3 / 2;

    ppa_client_config_t pcfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    if (ppa_register_client(&pcfg, &self->ppa) != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("castif: PPA client"));
    }
    uint32_t want_yuv = ((uint32_t)cw * ch * 3 / 2 + 127) & ~127u;
    self->yuv = esp_h264_aligned_calloc(128, 1, want_yuv, &self->yuv_len, ESP_H264_MEM_SPIRAM);
    if (!self->yuv) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: YUV buffer"));
    for (uint32_t i = 0; i + 2 < self->in_len; i += 3) {   // limited-range black
        self->yuv[i] = 0x80; self->yuv[i + 1] = 0x10; self->yuv[i + 2] = 0x10;
    }
    esp_h264_enc_cfg_hw_t cfg = {0};
    cfg.pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY;
    cfg.gop = args[ARG_gop].u_int;
    cfg.fps = args[ARG_fps].u_int;
    cfg.res.width = cw; cfg.res.height = ch;
    cfg.rc.bitrate = args[ARG_bitrate].u_int;
    cfg.rc.qp_min = args[ARG_qp_min].u_int;
    cfg.rc.qp_max = args[ARG_qp_max].u_int;
    castif_check(esp_h264_enc_hw_new(&cfg, &self->enc), "enc_hw_new");
    castif_check(esp_h264_enc_open(self->enc), "enc_open");
    self->open = true;
    castif_check(esp_h264_enc_hw_get_param_hd(self->enc, &self->param), "get_param");
    uint32_t want = args[ARG_out_size].u_int ? (uint32_t)args[ARG_out_size].u_int : AU_MAX;
    self->out_buf = esp_h264_aligned_calloc(16, 1, want, &self->out_cap, ESP_H264_MEM_SPIRAM);
    if (!self->out_buf) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: output buffer"));
    // PES header + access-unit delimiter, fixed
    static const uint8_t H[20] = {0,0,1,0xe0,0,0,0x80,0x80,5,0,0,0,0,0,0,0,0,1,9,0xf0};
    memcpy(self->pes_head, H, 20);
    self->ssrc = 0x50344341;
    self->seq = 1;
    self->audio_on = args[ARG_audio].u_bool;
    self->sync_limit = 20 * 90;
    if (self->audio_on) {
        uint32_t got = 0;
        self->audio_ring = esp_h264_aligned_calloc(16, 1, A_BLOCKS * A_BLOCK, &got, ESP_H264_MEM_SPIRAM);
        if (!self->audio_ring) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: audio ring"));
    }
    build_tables(self);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t castif_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    castif_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->task) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("already casting"));
    mp_buffer_info_t fb;
    mp_get_buffer_raise(pos_args[1], &fb, MP_BUFFER_READ);
    if (fb.len < (uint32_t)self->w * self->h * 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("framebuffer too small"));
    }
    const char *ip = mp_obj_str_get_str(pos_args[2]);
    int dst_port = mp_obj_get_int(pos_args[3]);
    int src_port = mp_obj_get_int(pos_args[4]);
    self->fb_obj = pos_args[1];
    self->fb = fb.buf;
    self->fb_len = fb.len;
    self->sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (self->sock < 0) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif: socket"));
    int one = 1;
    setsockopt(self->sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in src = {0};
    src.sin_family = AF_INET;
    src.sin_addr.s_addr = htonl(INADDR_ANY);
    src.sin_port = htons(src_port);
    if (bind(self->sock, (struct sockaddr *)&src, sizeof(src)) < 0) {
        closesocket(self->sock); self->sock = -1;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif: bind server_port"));
    }
    memset(&self->dst, 0, sizeof(self->dst));
    self->dst.sin_family = AF_INET;
    self->dst.sin_port = htons(dst_port);
    self->dst.sin_addr.s_addr = inet_addr(ip);
    self->rtp_n = 0;
    self->cc_pat = self->cc_pmt = self->cc_video = 0;
    self->frames = self->packets = self->sent = self->stalls = self->idr_reqs = 0;
    self->running = true;
    self->want_idr = true;   // start with a keyframe
    self->last_hash = 0; self->last_real_us = 0; self->skipped = 0;
    self->t0 = esp_timer_get_time();
    self->a_head = self->a_tail = 0; self->apts = 0; self->cc_audio = 0;
    self->drift = self->drift_ref = 0; self->drift_ref_set = false;
    self->audio_fed = self->audio_muxed = self->audio_underruns = 0;
    self->audio_inserted = self->audio_dropped = 0;
    if (self->max_skip_us == 0) self->max_skip_us = 500000;   // static UIs: >=2 fps floor
    // pin the framebuffer object so the GC does not move/free it while the task reads it
    // (a bytearray/Display buffer is long-lived; we also keep fb_obj referenced).
    xTaskCreatePinnedToCore(cast_task, "cast", 6144, self, 18, &self->task, 0);
    if (!self->task) {
        self->running = false;
        closesocket(self->sock); self->sock = -1;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("castif: task create"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(castif_start_obj, 5, castif_start);

static mp_obj_t castif_stop(mp_obj_t self_in) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->running) {
        self->running = false;
        for (int i = 0; i < 200 && self->task; i++) mp_hal_delay_ms(5);
    }
    self->fb_obj = MP_OBJ_NULL;
    self->fb = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_stop_obj, castif_stop);

static mp_obj_t castif_force_idr(mp_obj_t self_in) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->idr_reqs++;
    self->want_idr = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_force_idr_obj, castif_force_idr);

static mp_obj_t castif_set_bitrate(mp_obj_t self_in, mp_obj_t bps) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->new_bitrate = mp_obj_get_int(bps);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_set_bitrate_obj, castif_set_bitrate);

static mp_obj_t castif_feed_audio(mp_obj_t self_in, mp_obj_t block) {
    // one 10 ms block of 48 kHz stereo 16-bit big-endian LPCM into the ring;
    // False when the ring is full (the caller is ahead of real time)
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->audio_on) return mp_const_false;
    mp_buffer_info_t b;
    mp_get_buffer_raise(block, &b, MP_BUFFER_READ);
    if (b.len < A_BLOCK) mp_raise_ValueError(MP_ERROR_TEXT("audio block is 1920 bytes"));
    if (self->a_head - self->a_tail >= A_BLOCKS) return mp_const_false;
    uint32_t i = self->a_head % A_BLOCKS;
    memcpy(self->audio_ring + i * A_BLOCK, b.buf, A_BLOCK);
    self->a_stamp[i] = 90000 + (uint32_t)(((esp_timer_get_time() - self->t0) * 9) / 100);
    __sync_synchronize();
    self->a_head++;
    self->audio_fed++;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_feed_audio_obj, castif_feed_audio);

static mp_obj_t castif_set_sync(mp_obj_t self_in, mp_obj_t ms) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->sync_limit = (uint32_t)mp_obj_get_int(ms) * 90;   // 0 = measure the drift, never correct it
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_set_sync_obj, castif_set_sync);

static mp_obj_t castif_mark_dirty(mp_obj_t self_in) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->dirty = true;   // the panel changed: send the next frame, do not wait for the hash/floor
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_mark_dirty_obj, castif_mark_dirty);

static mp_obj_t castif_set_skip(mp_obj_t self_in, mp_obj_t ms) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->max_skip_us = (uint32_t)mp_obj_get_int(ms) * 1000;   // 0 = send every frame
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_set_skip_obj, castif_set_skip);

static mp_obj_t castif_set_fps(mp_obj_t self_in, mp_obj_t fps) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int f = mp_obj_get_int(fps);
    if (f < 1) f = 1;
    self->frame_us = 1000000 / f;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_set_fps_obj, castif_set_fps);

static mp_obj_t castif_stats(mp_obj_t self_in) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t d = mp_obj_new_dict(0);
    #define PUT(k, v) mp_obj_dict_store(d, MP_ROM_QSTR(k), mp_obj_new_int(v))
    PUT(MP_QSTR_frames, self->frames);
    PUT(MP_QSTR_fps, self->mfps);           // fps * 1000
    PUT(MP_QSTR_packets, self->packets);
    PUT(MP_QSTR_sent, self->sent);
    PUT(MP_QSTR_stalls, self->stalls);
    PUT(MP_QSTR_idr, self->idr_reqs);
    PUT(MP_QSTR_skipped, self->skipped);
    PUT(MP_QSTR_audio_fed, self->audio_fed);
    PUT(MP_QSTR_audio_muxed, self->audio_muxed);
    PUT(MP_QSTR_audio_underruns, self->audio_underruns);
    PUT(MP_QSTR_audio_level, self->a_head - self->a_tail);
    PUT(MP_QSTR_audio_inserted, self->audio_inserted);
    PUT(MP_QSTR_audio_dropped, self->audio_dropped);
    // how far the audio track has moved against the wall clock since it settled, ms (+ = early)
    PUT(MP_QSTR_audio_drift_ms, self->drift_ref_set ? (self->drift - self->drift_ref) / 90 : 0);
    PUT(MP_QSTR_enc_us, self->enc_us);
    PUT(MP_QSTR_ppa_us, self->ppa_us);
    PUT(MP_QSTR_mux_us, self->mux_us);
    PUT(MP_QSTR_length, self->last_len);
    PUT(MP_QSTR_frame_type, self->last_type);
    PUT(MP_QSTR_running, self->running);
    #undef PUT
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_stats_obj, castif_stats);

static mp_obj_t castif_close(mp_obj_t self_in) {
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    castif_stop(self_in);
    if (self->open) { esp_h264_enc_close(self->enc); esp_h264_enc_del(self->enc); self->open = false; }
    if (self->ppa) { ppa_unregister_client(self->ppa); self->ppa = NULL; }
    if (self->yuv) { esp_h264_free(self->yuv); self->yuv = NULL; }
    if (self->out_buf) { esp_h264_free(self->out_buf); self->out_buf = NULL; }
    if (self->audio_ring) { esp_h264_free(self->audio_ring); self->audio_ring = NULL; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_close_obj, castif_close);

static const mp_rom_map_elem_t castif_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&castif_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&castif_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_force_idr), MP_ROM_PTR(&castif_force_idr_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_bitrate), MP_ROM_PTR(&castif_set_bitrate_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_fps), MP_ROM_PTR(&castif_set_fps_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_skip), MP_ROM_PTR(&castif_set_skip_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark_dirty), MP_ROM_PTR(&castif_mark_dirty_obj) },
    { MP_ROM_QSTR(MP_QSTR_feed_audio), MP_ROM_PTR(&castif_feed_audio_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_sync), MP_ROM_PTR(&castif_set_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&castif_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&castif_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&castif_close_obj) },
};
static MP_DEFINE_CONST_DICT(castif_locals_dict, castif_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    castif_cast_type, MP_QSTR_Cast, MP_TYPE_FLAG_NONE,
    make_new, castif_make_new, locals_dict, &castif_locals_dict);

static const mp_rom_map_elem_t castif_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_castif) },
    { MP_ROM_QSTR(MP_QSTR_Cast), MP_ROM_PTR(&castif_cast_type) },
};
static MP_DEFINE_CONST_DICT(castif_module_globals, castif_module_globals_table);

const mp_obj_module_t castif_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&castif_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_castif, castif_user_cmodule);
