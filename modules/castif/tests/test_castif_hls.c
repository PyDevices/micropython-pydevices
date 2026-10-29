// SPDX-License-Identifier: MIT
// castif.Hls's segmenter, playlist and HTTP server, on a PC.
//
// From modules/castif:
//
//   cc -O2 -Wall -Wno-unused-function -Itests/host -I../tsmux/src -o test_castif_hls tests/test_castif_hls.c ../tsmux/src/tsmux_core.c
//   ./test_castif_hls OUTDIR          # exit 0 when every check passes
//   ./test_castif_hls OUTDIR --plant  # drops one keyframe: must fail
//
// It compiles src/castif_hls.c itself, everything but the encoder task and
// the Python API, against tsmux's real muxer, and feeds it tsmux's canned
// 160x96 stream (90 frames, a keyframe every 30) four times over at 30 fps.
// It writes what it serves to OUTDIR (each kept segment, the playlist and
// /stats) for check_castif_hls.py to hand to ffprobe and a JSON parser.

#define _GNU_SOURCE         // memmem
#define NO_QSTR
#define CASTIF_HOST_TEST
#include "../src/castif_hls.c"

#include <arpa/inet.h>
#include <fcntl.h>

#ifndef CANNED
#define CANNED "../tsmux/tests/canned_160x96.h264"
#endif

static int failures;

#define CHECK(cond, ...) do { \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
} while (0)

// --- the canned stream -------------------------------------------------------

typedef struct {
    const uint8_t *p;
    uint32_t len;
    bool idr;
} au_t;

static uint8_t *canned;
static au_t aus[256];
static int n_aus;

static bool au_is_idr(const uint8_t *p, uint32_t len) {
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1 && (p[i + 3] & 31) == 5) {
            return true;
        }
    }
    return false;
}

// Split at the access-unit delimiters, dropping each (tsmux writes its own).
static void load_canned(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("can't open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    canned = malloc(n);
    if (fread(canned, 1, n, f) != (size_t)n) {
        exit(2);
    }
    fclose(f);
    static const uint8_t aud[] = { 0, 0, 0, 1, 9 };
    long starts[256];
    int ns = 0;
    for (long i = 0; i + 5 <= n && ns < 256; i++) {
        if (memcmp(canned + i, aud, 5) == 0) {
            starts[ns++] = i;
        }
    }
    for (int k = 0; k < ns; k++) {
        long s = starts[k] + 6;
        long e = k + 1 < ns ? starts[k + 1] : n;
        aus[n_aus].p = canned + s;
        aus[n_aus].len = e - s;
        aus[n_aus].idr = au_is_idr(canned + s, e - s);
        n_aus++;
    }
}

// --- a fresh Hls, as make_new and start() leave it ----------------------------

static castif_hls_obj_t *new_hls(uint32_t target_ms, int listed, const char *path) {
    castif_hls_obj_t *h = calloc(1, sizeof(*h));
    h->target_ms = target_ms;
    h->target90 = target_ms * 90;
    h->target_s = (target_ms + 999) / 1000;
    h->listed = listed;
    snprintf(h->path, sizeof(h->path), "%s", path);
    h->mux = tsmux_new(0, hls_append, h);
    h->cl = calloc(HLS_CLIENTS, sizeof(hls_client_t));
    for (int i = 0; i < HLS_CLIENTS; i++) {
        h->cl[i].sock = -1;
    }
    h->ls = -1;
    return h;
}

// Feed the canned stream `loops` times at 30 fps (3000 ticks of 90 kHz a frame).
static uint32_t feed(castif_hls_obj_t *h, int loops, uint32_t pts, int skip_idr) {
    int idrs = 0;
    for (int l = 0; l < loops; l++) {
        for (int i = 0; i < n_aus; i++) {
            bool skip = aus[i].idr && ++idrs == skip_idr;
            if (!skip) {
                hls_frame(h, aus[i].p, aus[i].len, aus[i].idr, pts);
            }
            pts += 3000;
        }
    }
    return pts;
}

static int done_count(castif_hls_obj_t *h) {
    int n = 0;
    for (int i = 0; i < HLS_KEEP; i++) {
        n += h->seg[i].done;
    }
    return n;
}

static hls_seg_t *find_seg(castif_hls_obj_t *h, uint32_t seq) {
    for (int i = 0; i < HLS_KEEP; i++) {
        if (h->seg[i].done && h->seg[i].seq == seq) {
            return &h->seg[i];
        }
    }
    return NULL;
}

// --- one request, answered by hls_respond ------------------------------------

static hls_client_t *ask(castif_hls_obj_t *h, const char *req) {
    static hls_client_t c;
    if (c.seg) {
        c.seg->busy--;
    }
    memset(&c, 0, sizeof(c));
    c.sock = -1;
    c.ip = 0x0100007f;
    c.req_len = snprintf(c.req, sizeof(c.req), "%s", req);
    hls_respond(h, &c);
    return &c;
}

static bool head_has(hls_client_t *c, const char *s) {
    return strstr(c->head, s) != NULL;
}

static bool body_has(hls_client_t *c, const char *s) {
    char tmp[HLS_DYN + 1];
    uint32_t n = c->body_len < HLS_DYN ? c->body_len : HLS_DYN;
    memcpy(tmp, c->body, n);
    tmp[n] = 0;
    return strstr(tmp, s) != NULL;
}

static void save(const char *dir, const char *name, const void *p, uint32_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (f == NULL || fwrite(p, 1, len, f) != len) {
        printf("can't write %s\n", path);
        exit(2);
    }
    fclose(f);
}

// --- the checks --------------------------------------------------------------

static void test_canned(void) {
    int idrs = 0;
    for (int i = 0; i < n_aus; i++) {
        idrs += aus[i].idr;
    }
    CHECK(n_aus == 90 && idrs == 3 && aus[0].idr && aus[30].idr && aus[60].idr,
        "canned stream: %d access units, %d keyframes", n_aus, idrs);
}

// Segments cut at the first keyframe a target past the last, the newest eight
// kept, every one whole MPEG-TS that starts with the tables.
static void test_segments(const char *out, int plant) {
    castif_hls_obj_t *h = new_hls(1000, 3, "/run1");
    // a segment starts at a keyframe: the frames before the first are dropped
    hls_frame(h, aus[1].p, aus[1].len, false, 0);
    CHECK(h->cur == NULL && h->next_seq == 0, "a delta frame opened a segment");

    feed(h, 4, 3000, plant ? 2 : 0);
    // 12 keyframes, one a second: 11 finished segments and one open
    CHECK(h->next_seq == 12, "next_seq %u, want 12", (unsigned)h->next_seq);
    CHECK(h->cur != NULL && h->cur->open && h->cur->seq == 11, "the open segment isn't seq 11");
    CHECK(done_count(h) == 7, "%d finished segments kept, want 7 (eight slots, one open)", done_count(h));
    CHECK(h->overruns == 0, "%u overruns", (unsigned)h->overruns);
    CHECK(h->target_s == 1, "target duration %u, want 1", (unsigned)h->target_s);
    for (uint32_t seq = 4; seq <= 10; seq++) {
        hls_seg_t *s = find_seg(h, seq);
        CHECK(s != NULL, "segment %u not kept", (unsigned)seq);
        if (s == NULL) {
            continue;
        }
        CHECK(s->dur90 == 90000, "segment %u lasts %u ticks, want 90000", (unsigned)seq, (unsigned)s->dur90);
        CHECK(s->len > 0 && s->len % 188 == 0, "segment %u is %u bytes, not whole packets", (unsigned)seq, (unsigned)s->len);
        bool sync = true;
        for (uint32_t k = 0; k < s->len; k += 188) {
            sync = sync && s->buf[k] == 0x47;
        }
        CHECK(sync, "segment %u has a packet without 0x47", (unsigned)seq);
        // the first packet is the PAT (PID 0), so each segment decodes alone
        CHECK(s->len && (((s->buf[1] & 0x1f) << 8) | s->buf[2]) == 0, "segment %u doesn't start with the PAT", (unsigned)seq);
        CHECK(h->began_ring[seq & 511] == s->began_ms, "segment %u's start isn't in the ring", (unsigned)seq);
        char name[32];
        snprintf(name, sizeof(name), "seg%u.ts", (unsigned)seq);
        save(out, name, s->buf, s->len);
    }
    CHECK(find_seg(h, 3) == NULL, "segment 3 should have been reused");

    // the playlist: the newest `listed` (3), oldest first, at a fixed target
    char pl[HLS_DYN];
    int n = hls_playlist(h, pl, sizeof(pl), "");
    CHECK(n > 0 && n < (int)sizeof(pl), "playlist length %d", n);
    const char *want = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:8\n"
        "#EXTINF:1.000,\nseg8.ts\n#EXTINF:1.000,\nseg9.ts\n#EXTINF:1.000,\nseg10.ts\n";
    CHECK(strcmp(pl, want) == 0, "playlist:\n%s", pl);
    save(out, "stream.m3u8", pl, n);

    // a client sending the oldest keeps it from being reused
    hls_seg_t *oldest = find_seg(h, 4);
    if (oldest) {
        oldest->busy = 1;
        feed(h, 1, 3000 * 361, 0);      // three more keyframes: seqs 12-14
        CHECK(find_seg(h, 4) == oldest, "a segment being sent was reused");
        CHECK(find_seg(h, 5) == NULL, "segment 5 should have gone instead of 4");
        oldest->busy = 0;
    }
    free(h->cl);
    tsmux_free(h->mux);
    free(h);
}

// A segment that rounds above the target raises it, and it never falls.
static void test_target_rises(void) {
    castif_hls_obj_t *h = new_hls(500, 3, "");
    CHECK(h->target_s == 1, "target %u", (unsigned)h->target_s);
    hls_frame(h, aus[0].p, aus[0].len, true, 0);
    hls_frame(h, aus[0].p, aus[0].len, true, 90000 * 5 / 2 + 1);     // 2.5 s: rounds to 3
    CHECK(h->target_s == 3, "target %u after a 2.5 s segment, want 3", (unsigned)h->target_s);
    hls_frame(h, aus[0].p, aus[0].len, true, 90000 * 3 + 1);         // 0.5 s
    CHECK(h->target_s == 3 && h->next_seq == 3, "target fell to %u", (unsigned)h->target_s);
    // and a keyframe before the target doesn't cut
    uint32_t seq = h->cur->seq;
    hls_frame(h, aus[0].p, aus[0].len, true, 90000 * 3 + 9001);
    CHECK(h->cur->seq == seq, "a keyframe 0.1 s in cut the segment");
    tsmux_free(h->mux);
    free(h->cl);
    free(h);
}

static void test_i64(void) {
    char b[24];
    CHECK(strcmp(hls_i64(b, 0), "0") == 0, "0 -> %s", b);
    CHECK(strcmp(hls_i64(b, -42), "-42") == 0, "-42 -> %s", b);
    CHECK(strcmp(hls_i64(b, 1761700000123LL), "1761700000123") == 0, "-> %s", b);
    CHECK(strcmp(hls_i64(b, INT64_MAX), "9223372036854775807") == 0, "-> %s", b);
}

static void test_note_fetch(void) {
    castif_hls_obj_t *h = new_hls(1000, 3, "");
    hls_note_fetch(h, 1, 10);
    hls_note_fetch(h, 1, 11);       // the first fetch is the one kept
    for (uint32_t ip = 2; ip <= 5; ip++) {
        hls_note_fetch(h, ip, ip * 10);
    }
    CHECK(h->ips[0] == 1 && h->first_fetch[0] == 10, "ip 1's first fetch %u", (unsigned)h->first_fetch[0]);
    CHECK(h->ips[3] == 4 && h->first_fetch[3] == 40, "the fourth client");
    for (int i = 0; i < HLS_IPS; i++) {
        CHECK(h->ips[i] != 5, "a fifth client was remembered");
    }
    tsmux_free(h->mux);
    free(h->cl);
    free(h);
}

// What hls_respond answers for each path.
static void test_routes(const char *out) {
    castif_hls_obj_t *h = new_hls(1000, 3, "/run1");
    feed(h, 2, 0, 0);               // seqs 0-4 finished, 5 open
    hls_client_t *c;

    c = ask(h, "GET /stream.m3u8 HTTP/1.1\r\nHost: x\r\n\r\n");
    CHECK(head_has(c, "200 OK") && head_has(c, "application/vnd.apple.mpegurl"), "/stream.m3u8: %s", c->head);
    CHECK(body_has(c, "#EXT-X-MEDIA-SEQUENCE:2\n") && body_has(c, "seg4.ts\n"), "/stream.m3u8 body");

    c = ask(h, "GET /run1/stream.m3u8 HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "200 OK"), "/run1/stream.m3u8: %s", c->head);
    c = ask(h, "GET /run2/stream.m3u8 HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "404"), "another run's playlist was served");
    c = ask(h, "GET /stream.m3u8?t=5 HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "200 OK"), "a query string broke the playlist");

    c = ask(h, "GET /run1/seg3.ts HTTP/1.1\r\n\r\n");
    hls_seg_t *s3 = find_seg(h, 3);
    CHECK(head_has(c, "200 OK") && head_has(c, "video/mp2t"), "/run1/seg3.ts: %s", c->head);
    CHECK(s3 && c->body == s3->buf && c->body_len == s3->len && s3->busy == 1, "seg3's body or busy count");
    char cl[48];
    snprintf(cl, sizeof(cl), "Content-Length: %u\r\n", s3 ? (unsigned)s3->len : 0);
    CHECK(head_has(c, cl), "seg3's Content-Length");
    CHECK(h->ips[0] == 0x0100007f && h->first_fetch[0] == 3, "the first fetch wasn't noted");
    c = ask(h, "GET /seg4.ts HTTP/1.1\r\n\r\n");
    CHECK(s3 && s3->busy == 0, "seg3 still busy after its client went");
    CHECK(head_has(c, "200 OK") && h->first_fetch[0] == 3, "/seg4.ts");
    c = ask(h, "GET /seg5.ts HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "404") && c->body_len == 0, "the open segment was served");
    c = ask(h, "GET /seg99.ts HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "404"), "a missing segment was served");
    c = ask(h, "GET /xseg3.ts HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "404"), "/xseg3.ts was served");

    c = ask(h, "GET /video?state=play&t=\"1\\2\" HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "200 OK"), "/video: %s", c->head);
    CHECK(strcmp(h->player, "state=play&t=_1_2_") == 0, "player kept as %s", h->player);

    c = ask(h, "GET /stats HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "200 OK") && head_has(c, "application/json"), "/stats: %s", c->head);
    CHECK(body_has(c, "\"player\": \"state=play&t=_1_2_\"") && body_has(c, "\"first_fetch\": {\"127.0.0.1\": 3}"),
        "/stats body");
    save(out, "stats.json", c->body, c->body_len);

    c = ask(h, "GET /nothing HTTP/1.1\r\n\r\n");
    CHECK(head_has(c, "404 Not Found") && head_has(c, "Content-Length: 0\r\n"), "/nothing: %s", c->head);
    c = ask(h, "garbage");
    CHECK(head_has(c, "404"), "a malformed request");
    ask(h, "x");                    // drops the last client's busy count
    CHECK(h->requests == 14, "%u requests counted", (unsigned)h->requests);
    tsmux_free(h->mux);
    free(h->cl);
    free(h);
}

// The server end to end, over loopback: a client fetches a segment whole.
static void test_serve(void) {
    castif_hls_obj_t *h = new_hls(1000, 3, "");
    feed(h, 1, 0, 0);               // seqs 0-1 finished
    hls_seg_t *s = find_seg(h, 1);
    h->ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    CHECK(bind(h->ls, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(h->ls, 4) == 0, "listen");
    getsockname(h->ls, (struct sockaddr *)&a, &al);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(cs, (struct sockaddr *)&a, sizeof(a)) == 0, "connect");
    const char *req = "GET /seg1.ts HTTP/1.1\r\nHost: board\r\n\r\n";
    CHECK(send(cs, req, strlen(req), 0) == (ssize_t)strlen(req), "send");
    fcntl(cs, F_SETFL, O_NONBLOCK);
    uint32_t cap = 1 << 20, got = 0;
    uint8_t *buf = malloc(cap);
    bool closed = false;
    for (int round = 0; round < 2000 && !closed; round++) {
        hls_serve(h, 2);
        for (;;) {
            ssize_t r = recv(cs, buf + got, cap - got, 0);
            if (r > 0) {
                got += r;
            } else {
                closed = r == 0;
                break;
            }
        }
    }
    CHECK(closed, "the server never closed the connection");
    char *body = memmem(buf, got, "\r\n\r\n", 4);
    CHECK(body && memcmp(buf, "HTTP/1.1 200 OK\r\n", 17) == 0, "response head");
    if (body && s) {
        body += 4;
        uint32_t blen = got - (uint32_t)(body - (char *)buf);
        CHECK(blen == s->len && memcmp(body, s->buf, s->len) == 0, "served %u bytes, segment is %u", blen, s->len);
    }
    CHECK(s && s->busy == 0, "the segment stayed busy after its client closed");
    bool free_slots = true;
    for (int i = 0; i < HLS_CLIENTS; i++) {
        free_slots = free_slots && h->cl[i].sock < 0;
    }
    CHECK(free_slots, "a client slot wasn't freed");
    close(cs);
    close(h->ls);
    free(buf);
    tsmux_free(h->mux);
    free(h->cl);
    free(h);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: test_castif_hls OUTDIR [--plant]\n");
        return 2;
    }
    int plant = argc > 2 && strcmp(argv[2], "--plant") == 0;
    load_canned(CANNED);
    test_canned();
    test_segments(argv[1], plant);
    test_target_rises();
    test_i64();
    test_note_fetch();
    test_routes(argv[1]);
    test_serve();
    printf("%s: %d failure(s)%s\n", failures ? "FAILED" : "OK", failures, plant ? " (planted fault)" : "");
    return failures != 0;
}
