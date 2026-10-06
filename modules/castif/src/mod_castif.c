// castif: the cast as a FreeRTOS task on core 0 of the ESP32-P4.
//
// PPA convert and hardware H.264 encode (h264enc's C API), MPEG-TS mux
// (tsmux's) and RTP send run on core 0, off the interpreter, so the app keeps its frame rate on
// core 1 and a sink fed a steady stream foregrounds at once. The Wi-Fi Display session (MICE +
// RTSP) stays in Python and is idle after PLAY.
//
// Sound (audio=True): 10 ms LPCM blocks on the same wall clock as the video.
// Either Python feeds them (feed_audio), or the task reads the audio pump's
// output itself from an audiopump.Tap (set_tap), resampled to 48 kHz stereo,
// so nothing the interpreter does can starve the track.
//
//   c = castif.Cast(w, h, canvas_w=1280, canvas_h=720, fps=30, bitrate=3_000_000)
//   c.start(framebuffer, sink_ip, dst_port, src_port)   # after RTSP PLAY
//   c.stats(); c.force_idr(); c.set_bitrate(n); c.set_fps(n); c.stop()

#include <string.h>
#include "py/runtime.h"
#include "py/mphal.h"
#include "py/objstr.h"

#ifndef NO_QSTR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   // xTaskCreatePinnedToCore: FreeRTOS.h adds it only under ESP_PLATFORM
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_heap_caps.h"
#endif

// h264enc encodes: its C API (modules/h264enc/src/h264enc.h), declared here
// rather than included, and weak, so castif builds and links without h264enc
// in the firmware; Cast() then says what is missing.
typedef struct h264enc_session h264enc_session_t;
extern const char *h264enc_open(h264enc_session_t **out, int width, int height, int canvas_w, int canvas_h,
    int fps, int gop, int bitrate, int qp_min, int qp_max, uint32_t out_size) __attribute__((weak));
extern int h264enc_encode(h264enc_session_t *s, const uint8_t *rgb565, const uint8_t **data, uint32_t *len, bool *idr) __attribute__((weak));
extern void h264enc_force_idr(h264enc_session_t *s) __attribute__((weak));
extern void h264enc_set_bitrate(h264enc_session_t *s, uint32_t bps) __attribute__((weak));
extern void h264enc_timing(h264enc_session_t *s, uint32_t *ppa_us, uint32_t *enc_us) __attribute__((weak));
extern void h264enc_close(h264enc_session_t *s) __attribute__((weak));

// tsmux writes the MPEG-TS (modules/tsmux/src/tsmux_core.h), the same way:
// declared here, weak, and held by pointer.
typedef struct tsmux tsmux_t;
typedef void tsmux_out_fn(void *ctx, const uint8_t *pkt);
extern tsmux_t *tsmux_new(int lpcm, tsmux_out_fn *out, void *ctx) __attribute__((weak));
extern void tsmux_free(tsmux_t *m) __attribute__((weak));
extern void tsmux_reset(tsmux_t *m) __attribute__((weak));
extern void tsmux_tables(tsmux_t *m) __attribute__((weak));
extern void tsmux_video(tsmux_t *m, const uint8_t *au, uint32_t len, uint32_t pts, int key) __attribute__((weak));
extern void tsmux_lpcm(tsmux_t *m, const uint8_t *pcm, uint32_t len, uint32_t pts) __attribute__((weak));
extern void tsmux_pcr(tsmux_t *m, uint32_t pcr) __attribute__((weak));

// SPIRAM, 16-byte aligned (what esp_h264's allocator gave castif before).
static void *castif_spiram(size_t n) {
    return heap_caps_aligned_calloc(16, 1, n, MALLOC_CAP_SPIRAM);
}

#define PCR_LEAD  36000           // 400 ms in 90 kHz ticks
#define A_BLOCK   1920            // one 10 ms LPCM block: 480 frames * 4 bytes (48 kHz stereo s16)
#define A_BLOCKS  64              // ring depth: 640 ms of audio
#define TAP_CHUNK 4096            // bytes read from the pump's tap per call (85 ms of 24 kHz mono)
#define REJOIN    9000            // a gap past 100 ms re-anchors the track rather than padding it

// audiodsp's audiopump.Tap, read as a stream from this task (set_tap): its C
// entry points (audiopump_tap.h, audiodsp 0.6.2 and later). Weak, so a
// firmware without audiodsp still links; set_tap then says so.
extern uint32_t audiopump_tap_position(mp_obj_t tap) __attribute__((weak));
extern uint32_t audiopump_tap_frame_bytes(mp_obj_t tap) __attribute__((weak));
extern uint32_t audiopump_tap_read_since(mp_obj_t tap, uint32_t *cursor,
    uint8_t *dst, uint32_t max, bool *lapped) __attribute__((weak));

typedef struct _castif_obj_t {
    mp_obj_base_t base;
    // the encoder session (h264enc: PPA + esp_h264)
    h264enc_session_t *enc;
    uint16_t w, h, cw, ch;
    // source framebuffer (RGB565), pinned by start()
    mp_obj_t fb_obj;
    const uint8_t *fb;
    uint32_t fb_len;
    // TS mux
    tsmux_t *mux;                 // the TS (tsmux), its packets out through rtp_push
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
    // ahead of the video).
    bool audio_on;
    uint8_t *audio_ring;                 // A_BLOCKS * A_BLOCK, in SPIRAM
    volatile uint32_t a_head, a_tail;    // block write / read indices
    uint32_t apts;                        // audio PTS, 90 kHz
    int64_t t0;                           // esp_timer at start(): the wall clock both PTS come from
    uint32_t a_stamp[A_BLOCKS];           // wall-clock (90 kHz) when each block was fed
    int32_t drift, drift_ref;             // EMA of stamp - apts, and its value once settled
    bool drift_ref_set;
    uint32_t sync_limit;                  // drop/insert past this (90 kHz ticks); 0 = measure only
    int64_t next_audio_us;                // when the next 10 ms block goes out (the pacer)
    volatile uint32_t audio_fed, audio_muxed, audio_underruns, audio_inserted, audio_dropped;
    // the pump's output tap, read here in C (set_tap): no interpreter in the
    // audio path, so a garbage collection that holds Python for seconds
    // cannot starve the cast (it did, every ~3 minutes, 2026-09-27)
    volatile mp_obj_t tap;
    uint32_t tap_rate, tap_ch, tap_frame, tap_cursor;
    int32_t tap_prev_l, tap_prev_r;
    uint8_t *tap_in;                      // TAP_CHUNK bytes, SPIRAM
    int16_t acc[A_BLOCK / 2];             // a 48 kHz stereo block being filled
    uint32_t acc_n;                       // samples in acc
    volatile uint32_t tap_bytes, tap_lapped, tap_full;
    // resampling to exactly 48 kHz of wall clock: the P4's "24 kHz" I2S runs
    // 0.4 % slow (23904 Hz measured), and labelling it 24000 made the track
    // fall behind and the pacer pad a silent 10 ms block every 2 s (a click).
    uint32_t rs_step, rs_phase;           // input frames per output frame, and position, in 1/2^24
    uint32_t rs_nominal;                  // the step at the tap's nominal rate
    int64_t rs_t0, rs_last;               // the measurement window, and the last data seen
    uint64_t rs_frames;                   // input frames in the window
    int64_t rs_i;                         // the integral trim, in step units (1/2^24)
    volatile uint32_t tap_rate_x256;      // measured input rate, Hz * 256
    uint32_t since_anchor;                // blocks muxed since the track was (re)anchored
    volatile uint32_t audio_rejoins;
    volatile uint32_t enc_us, ppa_us, mux_us, send_us;
    volatile uint32_t last_len;
    volatile int last_type;
    volatile uint32_t mfps;       // measured fps * 1000
} castif_obj_t;

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

// tsmux's output: every TS packet into the RTP batch.
static void castif_ts_out(void *ctx, const uint8_t *pkt) {
    rtp_push((castif_obj_t *)ctx, pkt);
}

// ---- the streaming task (core 0) -----------------------------------------

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

// One block out (or one dropped: returns 0 then, so the caller goes again).
static int drain_one(castif_obj_t *c) {
    uint32_t i = c->a_tail % A_BLOCKS;
    if (c->apts == 0) {
        // The track starts so that the NEWEST waiting block carries the
        // clock time it was fed: a feeder that runs a few blocks ahead
        // (a burst at start, a lead after) then labels each block with
        // its production time, and a sound the app makes at T plays
        // beside the frame captured at T. Starting at the tick's PTS
        // instead put the audio a lead behind the picture (measured
        // +110 ms, 2026-09-27).
        uint32_t count = c->a_head - c->a_tail;
        uint32_t newest = c->a_stamp[(c->a_head - 1) % A_BLOCKS];
        c->apts = newest - 900 * (count - 1);
        c->drift = 0; c->drift_ref_set = false; c->since_anchor = 0;
    }
    int32_t d = (int32_t)(c->a_stamp[i] - c->apts);   // + : the track sits behind the clock
    // A gap (the producer stopped, or the ring ran dry) leaves the track
    // behind the clock by the gap's length. Padding that back 10 ms at a
    // time at real-time pace never catches up, and the ring overflows
    // meanwhile (measured: three 2.5 s gaps left the audio 5.6 s behind the
    // picture for good). Past REJOIN, re-anchor on this block's stamp: the
    // sink hears a gap, then audio beside its picture again.
    if (d - (c->drift_ref_set ? c->drift_ref : 0) > REJOIN) {
        c->apts = c->a_stamp[i];
        d = 0; c->drift = 0; c->drift_ref_set = false; c->since_anchor = 0;
        c->audio_rejoins++;
    }
    c->drift += (d - c->drift) / 64;
    if (!c->drift_ref_set) {
        if (c->since_anchor >= 300) { c->drift_ref = c->drift; c->drift_ref_set = true; }
    } else if (c->sync_limit) {
        int32_t off = c->drift - c->drift_ref;
        if (off > (int32_t)c->sync_limit) {          // audio would play early: pad
            tsmux_lpcm(c->mux, SILENCE, A_BLOCK, c->apts);
            c->apts += 900; c->audio_inserted++; c->drift -= 900;
            return 1;
        }
        if (off < -(int32_t)c->sync_limit) {         // audio would play late: drop one
            c->a_tail++; c->audio_dropped++; c->drift += 900;
            return 0;
        }
    }
    tsmux_lpcm(c->mux, c->audio_ring + i * A_BLOCK, A_BLOCK, c->apts);
    c->a_tail++; c->apts += 900; c->audio_muxed++; c->since_anchor++;
    return 1;
}

// One 48 kHz stereo block into the ring, stamped with the wall-clock time
// (90 kHz) its first sample was produced.
static void push_block(castif_obj_t *c, const uint8_t *blk, uint32_t stamp) {
    if (c->a_head - c->a_tail >= A_BLOCKS) { c->tap_full++; return; }
    uint32_t i = c->a_head % A_BLOCKS;
    memcpy(c->audio_ring + i * A_BLOCK, blk, A_BLOCK);
    c->a_stamp[i] = stamp;
    __sync_synchronize();
    c->a_head++;
    c->audio_fed++;
}

static inline void acc_frame(castif_obj_t *c, int32_t l, int32_t r, int64_t t_us) {
    c->acc[c->acc_n++] = (int16_t)l;
    c->acc[c->acc_n++] = (int16_t)r;
    if (c->acc_n == A_BLOCK / 2) {
        c->acc_n = 0;
        // t_us is when this, the block's LAST frame, was produced
        uint32_t stamp = 90000 + (uint32_t)(((t_us - c->t0) * 9) / 100) - 900;
        push_block(c, (const uint8_t *)c->acc, stamp);
    }
}

// Everything the pump has played since the last call, as 48 kHz stereo
// blocks. The newest byte in the tap went out about now; each earlier frame
// is dated back from it at the tap's rate. 24 kHz is doubled with a midpoint.
#define RS_ONE (1u << 24)

// The input rate, measured against esp_timer over a window that restarts
// after any gap, sets the step; the drift the pacer sees (stamp vs track)
// trims it, proportionally (0.1 % per 10 ms) and by an integral that grows
// 0.01 %/s per 10 ms, so a bias in the measured rate settles to zero drift
// rather than to a standing offset (a proportional trim alone crept to the
// 20 ms pad limit in 80 s, 2026-09-27). Clamped to 1 % of nominal.
static void rs_update(castif_obj_t *c, uint32_t frames, int64_t now) {
    if (c->rs_t0 == 0 || now - c->rs_last > 200000) {
        c->rs_t0 = now; c->rs_frames = 0;
    }
    c->rs_last = now;
    c->rs_frames += frames;
    int64_t el = now - c->rs_t0;
    uint64_t step = c->rs_nominal;
    if (el > 2000000) {
        uint64_t rate256 = c->rs_frames * 1000000ull * 256 / (uint64_t)el;
        c->tap_rate_x256 = (uint32_t)rate256;
        step = (rate256 << 24) / (48000ull * 256);
    }
    int64_t st = (int64_t)step;
    if (c->drift_ref_set) {
        int32_t off = c->drift - c->drift_ref;          // 90 kHz ticks; + = the track is behind
        int64_t lim = (int64_t)c->rs_nominal / 200;     // the integral holds at most 0.5 %
        c->rs_i -= st * off / 900000000;                // once per poll, ~100 polls a second
        if (c->rs_i > lim) c->rs_i = lim;
        if (c->rs_i < -lim) c->rs_i = -lim;
        st += c->rs_i - st * off / 900000;
    }
    int64_t lo = (int64_t)c->rs_nominal * 99 / 100, hi = (int64_t)c->rs_nominal * 101 / 100;
    if (st < lo) st = lo;
    if (st > hi) st = hi;
    c->rs_step = (uint32_t)st;
}

static void tap_poll(castif_obj_t *c, int64_t now) {
    mp_obj_t tap = c->tap;
    if (tap == MP_OBJ_NULL) return;
    for (int round = 0; round < 8; round++) {
        bool lapped = false;
        uint32_t n = audiopump_tap_read_since(tap, &c->tap_cursor, c->tap_in, TAP_CHUNK, &lapped);
        if (lapped) c->tap_lapped++;
        if (n == 0) return;
        c->tap_bytes += n;
        uint32_t frames = n / c->tap_frame;
        rs_update(c, frames, now);
        const int16_t *src = (const int16_t *)c->tap_in;
        int64_t behind = (int64_t)(audiopump_tap_position(tap) - c->tap_cursor) / c->tap_frame;
        for (uint32_t k = 0; k < frames; k++) {
            int32_t l, r;
            if (c->tap_ch == 1) { l = r = src[k]; } else { l = src[2 * k]; r = src[2 * k + 1]; }
            int64_t t = now - ((int64_t)(frames - 1 - k) + behind) * 1000000 / c->tap_rate;
            // linear interpolation from the previous input frame to this one
            int32_t pl = c->tap_prev_l, pr = c->tap_prev_r;
            while (c->rs_phase < RS_ONE) {
                int64_t f = c->rs_phase >> 8;             // 0..65535
                acc_frame(c, pl + (int32_t)(((int64_t)(l - pl) * f) >> 16),
                    pr + (int32_t)(((int64_t)(r - pr) * f) >> 16), t);
                c->rs_phase += c->rs_step;
            }
            c->rs_phase -= RS_ONE;
            c->tap_prev_l = l; c->tap_prev_r = r;
        }
        if (n < TAP_CHUNK) return;
    }
}

// The pacer: one block per 10 ms of wall clock, whatever the picture is
// doing, about 60 ms behind the feed so the session loop's 50 ms cadence
// never starves it. Audio then reaches the sink as a steady stream of 10 ms
// PES packets rather than in bursts once per video tick (up to 60 ms at a
// time at 16 fps), which is what a sink's clock recovery expects.
static void pace_audio(castif_obj_t *c, int64_t now) {
    tap_poll(c, now);
    if (c->a_tail == c->a_head) return;
    if (c->next_audio_us == 0) c->next_audio_us = now + 60000;
    if (now - c->next_audio_us > 300000) c->next_audio_us = now;   // a long gap: don't burst-chase
    int sent = 0;
    while (c->a_tail != c->a_head && now >= c->next_audio_us && sent < 16) {
        if (drain_one(c)) { c->next_audio_us += 10000; sent++; }
    }
    if (sent) rtp_flush(c);
}

// ---- the streaming task (core 0) -----------------------------------------

static void cast_task(void *arg) {
    castif_obj_t *c = (castif_obj_t *)arg;
    int64_t next = esp_timer_get_time();
    int64_t win_t0 = next;
    uint32_t win_frames = 0;
    while (c->running) {
        int64_t now = esp_timer_get_time();
        if (now < next) {
            if (c->audio_on) pace_audio(c, now);
            vTaskDelay(1);
            continue;
        }
        next += c->frame_us;
        if (now - next > 200000) next = now;        // don't chase a big backlog
        // controls
        if (c->new_bitrate > 0) {
            h264enc_set_bitrate(c->enc, (uint32_t)c->new_bitrate);
            c->new_bitrate = -1;
        }
        int idr = c->want_idr;
        if (idr) {
            h264enc_force_idr(c->enc);
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
        if (!skip) {
            const uint8_t *au;
            uint32_t au_len;
            bool key;
            int err = h264enc_encode(c->enc, c->fb, &au, &au_len, &key);
            uint32_t ppa_us, enc_us;
            h264enc_timing(c->enc, &ppa_us, &enc_us);
            c->ppa_us = ppa_us;
            c->enc_us = enc_us;
            if (err != 0) {
                skip = 1;
            } else {
                c->last_len = au_len;
                c->last_type = key ? 0 : 2;     // esp_h264's frame types: 0 IDR, 2 P
                m0 = esp_timer_get_time();
                if (key) tsmux_tables(c->mux);
                tsmux_video(c->mux, au, au_len, pts, key);
                c->frames++;
                win_frames++;
            }
        }
        if (c->audio_on) {
            if (skip) tsmux_pcr(c->mux, pts - PCR_LEAD);
            pace_audio(c, esp_timer_get_time());
            // the producer has stopped: the track is more than 200 ms behind the picture
            if (c->apts && (int32_t)(pts - c->apts) > 18000) c->audio_underruns++;
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
    self->max_skip_us = 500000;   // static UIs: >=2 fps floor; set_skip(0) turns it off
    self->frame_us = 1000000 / args[ARG_fps].u_int;
    if (tsmux_new == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif: needs the tsmux module in this firmware"));
    }
    if (h264enc_open == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif: needs the h264enc module in this firmware"));
    }
    const char *why = h264enc_open(&self->enc, w, h, cw, ch, args[ARG_fps].u_int, args[ARG_gop].u_int,
        args[ARG_bitrate].u_int, args[ARG_qp_min].u_int, args[ARG_qp_max].u_int, (uint32_t)args[ARG_out_size].u_int);
    if (why != NULL) {
        if (strcmp(why, "busy") == 0) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif: the H.264 encoder is busy (an h264enc.Encoder or another Cast has it)"));
        }
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("castif: %s"), why);
    }
    self->ssrc = 0x50344341;
    self->seq = 1;
    self->audio_on = args[ARG_audio].u_bool;
    self->sync_limit = 20 * 90;
    if (self->audio_on) {
        self->audio_ring = castif_spiram(A_BLOCKS * A_BLOCK);
        if (!self->audio_ring) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: audio ring"));
    }
    self->mux = tsmux_new(self->audio_on, castif_ts_out, self);
    if (self->mux == NULL) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: muxer"));
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
    tsmux_reset(self->mux);
    self->frames = self->packets = self->sent = self->stalls = self->idr_reqs = 0;
    self->running = true;
    self->want_idr = true;   // start with a keyframe
    self->last_hash = 0; self->last_real_us = 0; self->skipped = 0;
    self->t0 = esp_timer_get_time();
    self->a_head = self->a_tail = 0; self->apts = 0;
    self->drift = self->drift_ref = 0; self->drift_ref_set = false;
    self->next_audio_us = 0;
    self->audio_fed = self->audio_muxed = self->audio_underruns = 0;
    self->audio_inserted = self->audio_dropped = 0;
    self->since_anchor = 0; self->audio_rejoins = 0;
    self->tap_bytes = self->tap_lapped = self->tap_full = 0;
    self->acc_n = 0; self->tap_prev_l = self->tap_prev_r = 0;
    self->rs_phase = 0; self->rs_t0 = 0; self->rs_step = self->rs_nominal; self->rs_i = 0;
    if (self->tap != MP_OBJ_NULL) self->tap_cursor = audiopump_tap_position(self->tap);
    // pin the framebuffer object so the GC does not move/free it while the task reads it
    // (a bytearray/Display buffer is long-lived; we also keep fb_obj referenced).
    // 16 KB: the tap reader (64-bit rate maths) on top of lwip's sendto and the
    // encoder took the old 6 KB over the edge; stats()["stack_free"] shows the margin
    xTaskCreatePinnedToCore(cast_task, "cast", 16384, self, 18, &self->task, 0);
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
    // one 10 ms block of 48 kHz stereo 16-bit little-endian PCM (the pump's own bytes) into the ring;
    // False when the ring is full (the caller is ahead of real time)
    castif_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->audio_on || self->tap != MP_OBJ_NULL) return mp_const_false;   // one producer per ring
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

// set_tap(tap, rate, channels): the audio track comes from an audiopump.Tap,
// read by the cast task itself; set_tap(None) goes back to feed_audio().
static mp_obj_t castif_set_tap(size_t n_args, const mp_obj_t *args) {
    castif_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->audio_on) mp_raise_ValueError(MP_ERROR_TEXT("castif: made without audio=True"));
    if (!audiopump_tap_read_since) mp_raise_ValueError(MP_ERROR_TEXT("castif: this firmware has no audiodsp tap reader"));
    if (args[1] == mp_const_none) {
        if (self->running) mp_raise_ValueError(MP_ERROR_TEXT("castif: stop() before removing the tap"));
        self->tap = MP_OBJ_NULL;
        return mp_const_none;
    }
    uint32_t rate = n_args > 2 ? (uint32_t)mp_obj_get_int(args[2]) : 48000;
    uint32_t ch = n_args > 3 ? (uint32_t)mp_obj_get_int(args[3]) : 2;
    if ((rate != 24000 && rate != 48000) || (ch != 1 && ch != 2)) {
        mp_raise_ValueError(MP_ERROR_TEXT("castif: tap must be 24 or 48 kHz, mono or stereo"));
    }
    if (audiopump_tap_frame_bytes(args[1]) != ch * 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("castif: tap channel_count differs"));
    }
    if (!self->tap_in) {
        self->tap_in = castif_spiram(TAP_CHUNK);
        if (!self->tap_in) mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif: tap buffer"));
    }
    self->tap = MP_OBJ_NULL;
    __sync_synchronize();
    self->tap_rate = rate; self->tap_ch = ch; self->tap_frame = ch * 2;
    self->rs_nominal = self->rs_step = (uint32_t)(((uint64_t)rate << 24) / 48000);
    self->rs_phase = 0; self->rs_t0 = 0; self->rs_i = 0;
    self->acc_n = 0; self->tap_prev_l = self->tap_prev_r = 0;
    self->tap_cursor = audiopump_tap_position(args[1]);
    __sync_synchronize();
    self->tap = args[1];
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(castif_set_tap_obj, 2, 4, castif_set_tap);

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
    PUT(MP_QSTR_audio_rejoins, self->audio_rejoins);
    PUT(MP_QSTR_tap_bytes, self->tap_bytes);
    PUT(MP_QSTR_tap_lapped, self->tap_lapped);
    PUT(MP_QSTR_tap_full, self->tap_full);
    PUT(MP_QSTR_tap_hz, self->tap_rate_x256 / 256);
    PUT(MP_QSTR_trim_ppm, self->rs_nominal ? (int)(self->rs_i * 1000000 / self->rs_nominal) : 0);
    PUT(MP_QSTR_stack_free, self->task ? (int)uxTaskGetStackHighWaterMark(self->task) : -1);
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
    if (self->enc) { h264enc_close(self->enc); self->enc = NULL; }
    if (self->mux) { tsmux_free(self->mux); self->mux = NULL; }
    if (self->audio_ring) { heap_caps_free(self->audio_ring); self->audio_ring = NULL; }
    if (self->tap_in) { heap_caps_free(self->tap_in); self->tap_in = NULL; }
    self->tap = MP_OBJ_NULL;
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
    { MP_ROM_QSTR(MP_QSTR_set_tap), MP_ROM_PTR(&castif_set_tap_obj) },
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
