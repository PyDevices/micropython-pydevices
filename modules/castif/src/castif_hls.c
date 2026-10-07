// castif.Hls: live HLS from a framebuffer, as a FreeRTOS task on core 0.
//
//   hls = castif.Hls(1280, 720, fps=20, bitrate=2_000_000)
//   hls.start()                  # serving http://<board>:8090/stream.m3u8
//   hls.offer(framebuffer)       # after each finished frame: True if taken
//   hls.stats(); hls.stop(); hls.close()
//
// The task encodes the last frame offered (h264enc), forces a keyframe every
// `target` ms of wall clock, cuts a segment at each (tsmux), keeps the last
// eight in PSRAM, lists the newest `segments` in the playlist, and serves them
// all over HTTP. None of it needs the interpreter, so an app that holds it
// for a long redraw (an LVGL dial: ~330 ms) can't starve the stream -- the
// reason this is C (a Python thread did all this first, and skipped seconds).
//
// offer() is the only handover: it copies into a back buffer, and only when
// the task has taken the previous one, so the task never reads a frame that
// is still being drawn.
//
// Paths served: /stream.m3u8 and <path>/stream.m3u8 (a TV caches by URL, so a
// run can have its own), seg<N>.ts under either, /video?... (the Companion
// channel's player reports, kept for stats()), and /stats (JSON).

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include "py/runtime.h"
#include "py/objstr.h"

#ifndef NO_QSTR
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#endif

#include "castif_ext.h"

#define HLS_KEEP 8            // segments held (the playlist lists the newest few)
#define HLS_CLIENTS 6
#define HLS_REQ 1024
#define HLS_DYN 1536          // a playlist or /stats body
#define HLS_IPS 4             // clients remembered for first_fetch

typedef struct {
    uint8_t *buf;
    uint32_t len, cap;
    uint32_t seq;
    uint32_t dur90;           // finished segments: duration, 90 kHz
    uint32_t start_pts;
    int64_t began_ms;         // wall clock (epoch ms) of its first frame
    bool open, done;
    uint8_t busy;             // clients sending it
} hls_seg_t;

typedef struct {
    int sock;
    uint32_t ip;
    char req[HLS_REQ];
    int req_len;
    char head[192];
    int head_len, head_pos;
    const uint8_t *body;
    uint32_t body_len, body_pos;
    hls_seg_t *seg;           // body is this segment's buffer
    char dyn[HLS_DYN];        // body is this text
} hls_client_t;

typedef struct _castif_hls_obj_t {
    mp_obj_base_t base;
    h264enc_session_t *enc;
    tsmux_t *mux;
    uint16_t w, h;
    uint32_t frame_us, target_ms, target90, target_s;
    uint8_t listed;
    uint16_t port;
    char path[32];
    // the two frame buffers: the task encodes front, offer() fills back
    uint8_t *front, *back;
    uint32_t frame_bytes;
    volatile bool want, fresh;
    hls_seg_t seg[HLS_KEEP];
    hls_seg_t *cur;
    uint32_t next_seq;
    int ls;
    hls_client_t *cl;          // HLS_CLIENTS of them, in PSRAM
    uint32_t ips[HLS_IPS], first_fetch[HLS_IPS];
    // when each of the last 512 segments began (wall clock ms, by seq & 511):
    // a player's delay is now - (when its first segment began + its position)
    int64_t began_ring[512];
    char player[96];
    TaskHandle_t task;
    volatile bool running;
    volatile uint32_t frames, offers, requests, enc_us, ppa_us, overruns;
    int64_t t0;
} castif_hls_obj_t;

static int64_t hls_wall_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// --- segments -------------------------------------------------------------------

static void hls_append(void *ctx, const uint8_t *pkt) {
    castif_hls_obj_t *h = ctx;
    hls_seg_t *s = h->cur;
    if (s == NULL) {
        return;
    }
    if (s->len + 188 > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 256 * 1024;
        uint8_t *nb = heap_caps_realloc(s->buf, cap, MALLOC_CAP_SPIRAM);
        if (nb == NULL) {
            h->overruns++;          // out of PSRAM: the segment loses its tail
            return;
        }
        s->buf = nb;
        s->cap = cap;
    }
    memcpy(s->buf + s->len, pkt, 188);
    s->len += 188;
}

// The slot for a new segment: an empty one, else the oldest no one is sending.
static hls_seg_t *hls_slot(castif_hls_obj_t *h) {
    hls_seg_t *best = NULL;
    for (int i = 0; i < HLS_KEEP; i++) {
        hls_seg_t *s = &h->seg[i];
        if (!s->open && !s->done) {
            return s;
        }
        if (!s->open && !s->busy && (best == NULL || s->seq < best->seq)) {
            best = s;
        }
    }
    return best;
}

static void hls_frame(castif_hls_obj_t *h, const uint8_t *au, uint32_t len, bool idr, uint32_t pts) {
    hls_seg_t *s = h->cur;
    if (idr && s != NULL && (uint32_t)(pts - s->start_pts) >= h->target90) {
        s->dur90 = pts - s->start_pts;
        s->open = false;
        s->done = true;
        // the playlist's target duration: fixed, raised only if a segment
        // rounds above it (HLS forbids it to change; tsmux.Segmenter's rule)
        uint32_t secs = (s->dur90 + 45000) / 90000;
        if (secs > h->target_s) {
            h->target_s = secs;
        }
        h->cur = s = NULL;
    }
    if (s == NULL) {
        if (!idr) {
            return;                     // a segment starts at a keyframe
        }
        s = hls_slot(h);
        if (s == NULL) {
            h->overruns++;
            return;
        }
        s->len = 0;
        s->done = false;
        s->open = true;
        s->seq = h->next_seq++;
        s->start_pts = pts;
        s->began_ms = hls_wall_ms();
        h->began_ring[s->seq & 511] = s->began_ms;
        h->cur = s;
    }
    if (idr) {
        tsmux_tables(h->mux);
    }
    tsmux_video(h->mux, au, len, pts, idr);
}

// --- HTTP -------------------------------------------------------------------------

static void hls_client_close(hls_client_t *c) {
    if (c->sock >= 0) {
        closesocket(c->sock);
    }
    if (c->seg) {
        c->seg->busy--;
    }
    c->sock = -1;
    c->seg = NULL;
    c->req_len = c->head_len = c->head_pos = 0;
    c->body = NULL;
    c->body_len = c->body_pos = 0;
}

static int hls_playlist(castif_hls_obj_t *h, char *out, int cap, const char *prefix) {
    // the newest `listed` finished segments, oldest first
    hls_seg_t *list[HLS_KEEP];
    int n = 0;
    for (int i = 0; i < HLS_KEEP; i++) {
        if (h->seg[i].done) {
            list[n++] = &h->seg[i];
        }
    }
    for (int i = 1; i < n; i++) {           // by sequence number
        for (int j = i; j > 0 && list[j - 1]->seq > list[j]->seq; j--) {
            hls_seg_t *t = list[j];
            list[j] = list[j - 1];
            list[j - 1] = t;
        }
    }
    int from = n > h->listed ? n - h->listed : 0;
    int k = snprintf(out, cap, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%u\n#EXT-X-MEDIA-SEQUENCE:%u\n",
        (unsigned)h->target_s, (unsigned)(n ? list[from]->seq : 0));
    for (int i = from; i < n && k < cap - 64; i++) {
        uint32_t ms = list[i]->dur90 / 90;
        k += snprintf(out + k, cap - k, "#EXTINF:%u.%03u,\n%sseg%u.ts\n", (unsigned)(ms / 1000), (unsigned)(ms % 1000),
            prefix, (unsigned)list[i]->seq);
    }
    return k;
}

// A 64-bit count in decimal. The firmware's printf is newlib's nano one,
// which has no %lld: it reads the next argument from the wrong slot, and a %s
// after it took 0x3bc5 for a string and faulted the task (2026-10-06).
static const char *hls_i64(char *buf, int64_t v) {
    uint64_t u = v < 0 ? (uint64_t)-v : (uint64_t)v;
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = '0' + (char)(u % 10);
        u /= 10;
    } while (u);
    int k = 0;
    if (v < 0) {
        buf[k++] = '-';
    }
    while (n) {
        buf[k++] = tmp[--n];
    }
    buf[k] = 0;
    return buf;
}

static int hls_stats_json(castif_hls_obj_t *h, char *out, int cap) {
    char num[24];
    int k = snprintf(out, cap, "{\"now\": %s, \"frames\": %u, \"offers\": %u, \"requests\": %u, \"enc_us\": %u, \"ppa_us\": %u, "
        "\"overruns\": %u, \"player\": \"%s\", \"began\": {",
        hls_i64(num, hls_wall_ms()), (unsigned)h->frames, (unsigned)h->offers, (unsigned)h->requests,
        (unsigned)h->enc_us, (unsigned)h->ppa_us, (unsigned)h->overruns, h->player);
    bool first = true;
    for (int i = 0; i < HLS_KEEP; i++) {
        hls_seg_t *s = &h->seg[i];
        if (s->open || s->done) {
            k += snprintf(out + k, cap - k, "%s\"%u\": %s", first ? "" : ", ", (unsigned)s->seq, hls_i64(num, s->began_ms));
            first = false;
        }
    }
    // each client's first segment, and when it began
    for (int pass = 0; pass < 2; pass++) {
        k += snprintf(out + k, cap - k, pass ? "}, \"first_began\": {" : "}, \"first_fetch\": {");
        first = true;
        for (int i = 0; i < HLS_IPS; i++) {
            if (h->ips[i]) {
                uint32_t ip = h->ips[i];
                k += snprintf(out + k, cap - k, "%s\"%u.%u.%u.%u\": ", first ? "" : ", ",
                    (unsigned)(ip & 255), (unsigned)((ip >> 8) & 255), (unsigned)((ip >> 16) & 255), (unsigned)(ip >> 24));
                if (pass) {
                    k += snprintf(out + k, cap - k, "%s", hls_i64(num, h->began_ring[h->first_fetch[i] & 511]));
                } else {
                    k += snprintf(out + k, cap - k, "%u", (unsigned)h->first_fetch[i]);
                }
                first = false;
            }
        }
    }
    k += snprintf(out + k, cap - k, "}}");
    return k;
}

static void hls_note_fetch(castif_hls_obj_t *h, uint32_t ip, uint32_t seq) {
    for (int i = 0; i < HLS_IPS; i++) {
        if (h->ips[i] == ip) {
            return;
        }
    }
    for (int i = 0; i < HLS_IPS; i++) {
        if (h->ips[i] == 0) {
            h->ips[i] = ip;
            h->first_fetch[i] = seq;
            return;
        }
    }
}

static bool hls_ends_with(const char *s, int n, const char *tail) {
    int t = strlen(tail);
    return n >= t && memcmp(s + n - t, tail, t) == 0;
}

static void hls_respond(castif_hls_obj_t *h, hls_client_t *c) {
    h->requests++;
    // "GET /path HTTP/1.1"
    char *p = memchr(c->req, ' ', c->req_len);
    char *path = p ? p + 1 : c->req;
    char *end = memchr(path, ' ', c->req + c->req_len - path);
    int n = end ? (int)(end - path) : 0;
    char *q = memchr(path, '?', n);
    int plen = q ? (int)(q - path) : n;
    const char *ctype = "text/plain";
    const char *status = "404 Not Found";
    int pl = strlen(h->path);
    if (plen >= 6 && memcmp(path, "/video", 6) == 0) {
        int ql = q ? n - (int)(q - path) - 1 : 0;
        if (ql > (int)sizeof(h->player) - 1) {
            ql = sizeof(h->player) - 1;
        }
        for (int i = 0; i < ql; i++) {      // kept for stats(), JSON-safe
            char ch = q[1 + i];
            h->player[i] = (ch == '"' || ch == '\\' || ch < 32) ? '_' : ch;
        }
        h->player[ql] = 0;
        status = "200 OK";
    } else if (plen == 6 && memcmp(path, "/stats", 6) == 0) {
        c->body_len = hls_stats_json(h, c->dyn, HLS_DYN);
        c->body = (const uint8_t *)c->dyn;
        ctype = "application/json";
        status = "200 OK";
    } else if (hls_ends_with(path, plen, "/stream.m3u8") &&
               (plen == 12 || (plen == pl + 12 && memcmp(path, h->path, pl) == 0))) {
        c->body_len = hls_playlist(h, c->dyn, HLS_DYN, "");
        c->body = (const uint8_t *)c->dyn;
        ctype = "application/vnd.apple.mpegurl";
        status = "200 OK";
    } else if (hls_ends_with(path, plen, ".ts")) {
        // .../seg<N>.ts
        int i = plen - 4;
        while (i > 0 && path[i - 1] >= '0' && path[i - 1] <= '9') {
            i--;
        }
        if (i >= 4 && memcmp(path + i - 4, "/seg", 4) == 0) {
            uint32_t seq = 0;
            for (int j = i; j < plen - 3; j++) {
                seq = seq * 10 + (path[j] - '0');
            }
            for (int k = 0; k < HLS_KEEP; k++) {
                hls_seg_t *s = &h->seg[k];
                if (s->done && s->seq == seq) {
                    c->seg = s;
                    s->busy++;
                    c->body = s->buf;
                    c->body_len = s->len;
                    ctype = "video/mp2t";
                    status = "200 OK";
                    hls_note_fetch(h, c->ip, seq);
                    break;
                }
            }
        }
    }
    c->head_len = snprintf(c->head, sizeof(c->head),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n",
        status, ctype, (unsigned)c->body_len);
    c->head_pos = 0;
    c->body_pos = 0;
}

// Read, answer, send what the socket takes. false when the client is done.
static bool hls_client_step(castif_hls_obj_t *h, hls_client_t *c) {
    if (c->head_len == 0) {
        int r = recv(c->sock, c->req + c->req_len, HLS_REQ - 1 - c->req_len, MSG_DONTWAIT);
        if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            return false;
        }
        if (r > 0) {
            c->req_len += r;
            c->req[c->req_len] = 0;
        }
        if (strstr(c->req, "\r\n\r\n") || c->req_len >= HLS_REQ - 1) {
            hls_respond(h, c);
        } else {
            return true;
        }
    }
    while (c->head_pos < c->head_len) {
        int r = send(c->sock, c->head + c->head_pos, c->head_len - c->head_pos, MSG_DONTWAIT);
        if (r <= 0) {
            return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        c->head_pos += r;
    }
    while (c->body_pos < c->body_len) {
        uint32_t chunk = c->body_len - c->body_pos;
        if (chunk > 16384) {
            chunk = 16384;
        }
        int r = send(c->sock, c->body + c->body_pos, chunk, MSG_DONTWAIT);
        if (r <= 0) {
            return r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
        c->body_pos += r;
    }
    return false;               // all sent
}

static void hls_serve(castif_hls_obj_t *h, int wait_ms) {
    fd_set rd, wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int maxfd = h->ls;
    FD_SET(h->ls, &rd);
    for (int i = 0; i < HLS_CLIENTS; i++) {
        hls_client_t *c = &h->cl[i];
        if (c->sock < 0) {
            continue;
        }
        if (c->head_len == 0) {
            FD_SET(c->sock, &rd);
        } else {
            FD_SET(c->sock, &wr);
        }
        if (c->sock > maxfd) {
            maxfd = c->sock;
        }
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = wait_ms * 1000 };
    if (select(maxfd + 1, &rd, &wr, NULL, &tv) <= 0) {
        return;
    }
    if (FD_ISSET(h->ls, &rd)) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int s = accept(h->ls, (struct sockaddr *)&from, &fl);
        if (s >= 0) {
            hls_client_t *slot = NULL;
            for (int i = 0; i < HLS_CLIENTS && !slot; i++) {
                if (h->cl[i].sock < 0) {
                    slot = &h->cl[i];
                }
            }
            if (slot == NULL) {
                closesocket(s);
            } else {
                int one = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                slot->sock = s;
                slot->ip = from.sin_addr.s_addr;
            }
        }
    }
    for (int i = 0; i < HLS_CLIENTS; i++) {
        hls_client_t *c = &h->cl[i];
        if (c->sock >= 0 && (FD_ISSET(c->sock, &rd) || FD_ISSET(c->sock, &wr))) {
            if (!hls_client_step(h, c)) {
                hls_client_close(c);
            }
        }
    }
}

// --- the task (core 0) ----------------------------------------------------------

static void hls_task(void *arg) {
    castif_hls_obj_t *h = arg;
    int64_t next = esp_timer_get_time();
    int64_t last_key = next - (int64_t)h->target_ms * 1000;
    int64_t last_enc = next - 1000000;
    bool changed = true;
    h->t0 = next;
    while (h->running) {
        int64_t now = esp_timer_get_time();
        if (now >= next) {
            next += h->frame_us;
            if (now - next > 1000000) {
                next = now;                         // far behind: don't chase it
            }
            if (h->fresh) {                         // offer() filled back: swap
                uint8_t *t = h->front;
                h->front = h->back;
                h->back = t;
                h->fresh = false;
                __sync_synchronize();
                h->want = true;
                changed = true;
            }
            bool key = now - last_key >= (int64_t)h->target_ms * 1000;
            // Nothing new: encode the same picture only 5 times a second (and
            // for each keyframe). Each encode reads the whole framebuffer from
            // PSRAM through the PPA; at 15 fps of an unchanged 1280x720 frame
            // that traffic slowed an LVGL app's drawing and our own sends.
            if (!changed && !key && now - last_enc < 200000) {
                goto serve;
            }
            if (key) {
                h264enc_force_idr(h->enc);          // a segment every target ms, whatever the fps
                last_key = now;
            }
            changed = false;
            last_enc = now;
            const uint8_t *au;
            uint32_t len;
            bool idr;
            if (h264enc_encode(h->enc, h->front, &au, &len, &idr) == 0) {
                uint32_t ppa, enc;
                h264enc_timing(h->enc, &ppa, &enc);
                h->ppa_us = ppa;
                h->enc_us = enc;
                hls_frame(h, au, len, idr, 90000 + (uint32_t)(((now - h->t0) * 9) / 100));
                h->frames++;
            }
        }
    serve:;
        int64_t left = (next - esp_timer_get_time()) / 1000;
        hls_serve(h, left < 1 ? 1 : (left > 20 ? 20 : (int)left));
    }
    for (int i = 0; i < HLS_CLIENTS; i++) {
        if (h->cl[i].sock >= 0) {
            hls_client_close(&h->cl[i]);
        }
    }
    h->task = NULL;
    vTaskDelete(NULL);
}

// --- Python API -------------------------------------------------------------------

static void hls_free(castif_hls_obj_t *h) {
    if (h->enc) {
        h264enc_close(h->enc);
        h->enc = NULL;
    }
    if (h->mux) {
        tsmux_free(h->mux);
        h->mux = NULL;
    }
    for (int i = 0; i < HLS_KEEP; i++) {
        if (h->seg[i].buf) {
            heap_caps_free(h->seg[i].buf);
        }
        memset(&h->seg[i], 0, sizeof(hls_seg_t));
    }
    if (h->front) {
        heap_caps_free(h->front);
        h->front = NULL;
    }
    if (h->back) {
        heap_caps_free(h->back);
        h->back = NULL;
    }
    if (h->cl) {
        heap_caps_free(h->cl);
        h->cl = NULL;
    }
    if (h->ls >= 0) {
        closesocket(h->ls);
        h->ls = -1;
    }
}

static mp_obj_t castif_hls_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_width, ARG_height, ARG_fps, ARG_bitrate, ARG_segments, ARG_target, ARG_port, ARG_path };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_fps, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 20} },
        { MP_QSTR_bitrate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 2000000} },
        { MP_QSTR_segments, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 3} },
        { MP_QSTR_target, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 900} },
        { MP_QSTR_port, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 8090} },
        { MP_QSTR_path, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (h264enc_open == NULL || tsmux_new == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif.Hls needs h264enc and tsmux in this firmware"));
    }
    mp_int_t fps = args[ARG_fps].u_int, seg = args[ARG_segments].u_int, target = args[ARG_target].u_int;
    if (fps < 1 || fps > 60 || seg < 1 || seg > HLS_KEEP - 2 || target < 300 || target > 10000) {
        mp_raise_ValueError(MP_ERROR_TEXT("castif.Hls: fps 1..60, segments 1..6, target 300..10000 ms"));
    }
    castif_hls_obj_t *h = mp_obj_malloc_with_finaliser(castif_hls_obj_t, type);
    memset((char *)h + sizeof(mp_obj_base_t), 0, sizeof(*h) - sizeof(mp_obj_base_t));
    h->ls = -1;
    h->w = args[ARG_width].u_int;
    h->h = args[ARG_height].u_int;
    h->frame_us = 1000000 / fps;
    h->target_ms = target;
    h->target90 = target * 90;
    h->target_s = (target + 500) / 1000 ? (target + 500) / 1000 : 1;
    h->listed = seg;
    h->port = args[ARG_port].u_int;
    if (args[ARG_path].u_obj != mp_const_none) {
        size_t l;
        const char *p = mp_obj_str_get_data(args[ARG_path].u_obj, &l);
        if (l >= sizeof(h->path) || (l && p[0] != '/')) {
            mp_raise_ValueError(MP_ERROR_TEXT("castif.Hls: path is '/...', under 32 characters"));
        }
        memcpy(h->path, p, l);
    }
    // h264enc checks the size; a keyframe is forced by the clock, so the GOP is the longest allowed
    const char *why = h264enc_open(&h->enc, h->w, h->h, 0, 0, fps, 255, args[ARG_bitrate].u_int, 10, 45, 0);
    if (why != NULL) {
        h->enc = NULL;
        if (strcmp(why, "busy") == 0) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif.Hls: the H.264 encoder is busy (a cast or an h264enc.Encoder has it)"));
        }
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("castif.Hls: %s"), why);
    }
    h->mux = tsmux_new(0, hls_append, h);
    h->frame_bytes = (uint32_t)h->w * h->h * 2;
    h->front = heap_caps_calloc(1, h->frame_bytes, MALLOC_CAP_SPIRAM);
    h->back = heap_caps_calloc(1, h->frame_bytes, MALLOC_CAP_SPIRAM);
    h->cl = heap_caps_calloc(HLS_CLIENTS, sizeof(hls_client_t), MALLOC_CAP_SPIRAM);
    if (!h->mux || !h->front || !h->back || !h->cl) {
        hls_free(h);
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("castif.Hls: buffers"));
    }
    for (int i = 0; i < HLS_CLIENTS; i++) {
        h->cl[i].sock = -1;
    }
    h->want = true;
    return MP_OBJ_FROM_PTR(h);
}

static mp_obj_t castif_hls_start(mp_obj_t self_in) {
    castif_hls_obj_t *h = MP_OBJ_TO_PTR(self_in);
    if (h->task) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("castif.Hls: already running"));
    }
    if (!h->enc) {
        mp_raise_ValueError(MP_ERROR_TEXT("castif.Hls: closed"));
    }
    h->ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(h->ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(h->port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (h->ls < 0 || bind(h->ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(h->ls, 4) != 0) {
        if (h->ls >= 0) {
            closesocket(h->ls);
        }
        h->ls = -1;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif.Hls: can't listen on that port"));
    }
    h->running = true;
    // core 0, beside castif's cast task (which needed 16 KB with its audio
    // maths); lwip's select, the encoder call and snprintf fit in 12
    xTaskCreatePinnedToCore(hls_task, "hls", 12288, h, 17, &h->task, 0);
    if (!h->task) {
        h->running = false;
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("castif.Hls: no task"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_hls_start_obj, castif_hls_start);

// offer(framebuffer) -> True when taken: the next frames encode it
static mp_obj_t castif_hls_offer(mp_obj_t self_in, mp_obj_t fb) {
    castif_hls_obj_t *h = MP_OBJ_TO_PTR(self_in);
    if (!h->want || !h->back) {
        return mp_const_false;          // the task hasn't taken the last one yet
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(fb, &b, MP_BUFFER_READ);
    if (b.len < h->frame_bytes) {
        mp_raise_ValueError(MP_ERROR_TEXT("castif.Hls: framebuffer too small"));
    }
    h->want = false;
    memcpy(h->back, b.buf, h->frame_bytes);
    __sync_synchronize();
    h->fresh = true;
    h->offers++;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(castif_hls_offer_obj, castif_hls_offer);

static mp_obj_t castif_hls_stop(mp_obj_t self_in) {
    castif_hls_obj_t *h = MP_OBJ_TO_PTR(self_in);
    if (h->task) {
        h->running = false;
        for (int i = 0; i < 200 && h->task; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (h->ls >= 0) {
        closesocket(h->ls);
        h->ls = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_hls_stop_obj, castif_hls_stop);

static mp_obj_t castif_hls_close(mp_obj_t self_in) {
    castif_hls_obj_t *h = MP_OBJ_TO_PTR(self_in);
    castif_hls_stop(self_in);
    hls_free(h);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_hls_close_obj, castif_hls_close);

static mp_obj_t castif_hls_stats(mp_obj_t self_in) {
    castif_hls_obj_t *h = MP_OBJ_TO_PTR(self_in);
    mp_obj_t d = mp_obj_new_dict(0);
    #define PUT(k, v) mp_obj_dict_store(d, MP_ROM_QSTR(k), mp_obj_new_int_from_uint(v))
    PUT(MP_QSTR_frames, h->frames);
    PUT(MP_QSTR_offers, h->offers);
    PUT(MP_QSTR_requests, h->requests);
    PUT(MP_QSTR_enc_us, h->enc_us);
    PUT(MP_QSTR_ppa_us, h->ppa_us);
    PUT(MP_QSTR_overruns, h->overruns);
    PUT(MP_QSTR_running, h->task != NULL);
    #undef PUT
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_player), mp_obj_new_str(h->player, strlen(h->player)));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(castif_hls_stats_obj, castif_hls_stats);

static const mp_rom_map_elem_t castif_hls_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&castif_hls_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_offer), MP_ROM_PTR(&castif_hls_offer_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&castif_hls_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats), MP_ROM_PTR(&castif_hls_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&castif_hls_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&castif_hls_close_obj) },
};
static MP_DEFINE_CONST_DICT(castif_hls_locals_dict, castif_hls_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    castif_hls_type, MP_QSTR_Hls, MP_TYPE_FLAG_NONE,
    make_new, castif_hls_make_new, locals_dict, &castif_hls_locals_dict);
