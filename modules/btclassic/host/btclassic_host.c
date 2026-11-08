// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's loopback backend, for the unix port: no radio, and a phone
// played by the test. The radio turns on and off, the sink and the
// hands-free unit start and stop, and their audio runs through the same
// rings (../esp-idf/btclassic/ring.c) a board's does. `_btclassic_host`
// is the phone's side:
//
//     _btclassic_host.stream(pcm, rate, speed=1.0)  # loop pcm into the sink
//     _btclassic_host.stream(None)                  # and stop
//     _btclassic_host.call(rate, voice, mic)  # call audio up at 8000 or 16000
//     _btclassic_host.call(0)                 # and down
//     _btclassic_host.sent(buf)           # what the board queued for the phone
//
// Each plays from a thread of its own, the way Bluedroid's task writes on a
// board: nothing the interpreter does holds it up. The stream loops `pcm`
// at `rate` times `speed`; `speed` a little off 1.0 is a sender whose clock
// disagrees with the output's. A call loops `voice` (16-bit mono) into the
// call's ring as the phone's voice, and `mic` into a capture ring that
// HandsFree.uplink() takes from, standing in for the board's microphone.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "py/obj.h"
#include "py/runtime.h"

#include "btclassic.h"
#include "ring.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)

// ---- the radio ----

static struct {
    bool active, discoverable, connectable;
    char name[249];
    char pin[17];
    uint32_t changes;
} radio = { .connectable = true, .discoverable = true, .pin = "0000" };

int btc_radio_start(void) {
    radio.active = true;
    return BTC_OK;
}

void btc_radio_stop(void) {
    radio.active = false;
}

bool btc_radio_active(void) {
    return radio.active;
}

static size_t copy_text(char *dst, size_t size, const char *src) {
    size_t n = strlen(src);
    if (size == 0) {
        return 0;
    }
    if (n >= size) {
        n = size - 1;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
    return n;
}

int btc_radio_set_name(const char *name) {
    if (strlen(name) >= sizeof(radio.name)) {
        return BTC_ERR_ARG;
    }
    strcpy(radio.name, name);
    return BTC_OK;
}

size_t btc_radio_name(char *dst, size_t size) {
    return copy_text(dst, size, radio.name);
}

int btc_radio_set_visibility(bool discoverable, bool connectable) {
    radio.discoverable = discoverable;
    radio.connectable = connectable;
    return BTC_OK;
}

void btc_radio_visibility(bool *discoverable, bool *connectable) {
    *discoverable = radio.discoverable;
    *connectable = radio.connectable;
}

int btc_radio_set_pin(const char *pin) {
    size_t n = strlen(pin);
    if (n < 1 || n >= sizeof(radio.pin)) {
        return BTC_ERR_ARG;
    }
    strcpy(radio.pin, pin);
    return BTC_OK;
}

size_t btc_radio_pin(char *dst, size_t size) {
    return copy_text(dst, size, radio.pin);
}

int btc_radio_mac(btc_addr_t mac) {
    if (!radio.active) {
        return BTC_ERR_STATE;
    }
    static const btc_addr_t ours = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
    memcpy(mac, ours, 6);
    return BTC_OK;
}

int btc_radio_paired(btc_addr_t *addrs, int max) {
    (void)addrs;
    (void)max;
    return 0;
}

int btc_radio_unpair(const uint8_t *addr) {
    (void)addr;
    return BTC_OK;
}

uint32_t btc_radio_changes(void) {
    uint32_t c = radio.changes;
    radio.changes = 0;
    return c;
}

// ---- the sink ----

static const btc_addr_t phone = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };

static struct {
    bool started;
    btc_ring_t ring;
    int16_t *buf;
    uint8_t volume;
    uint32_t changes;
    volatile bool streaming;
    volatile uint32_t rate;
    // The phone's sending thread.
    pthread_t thread;
    bool thread_live;
    volatile bool thread_stop;
    int16_t *pcm;
    uint32_t pcm_frames;
    double speed;
} sink;

static void sink_mark(uint32_t bits) {
    LOCK();
    sink.changes |= bits;
    UNLOCK();
}

int btc_sink_start(size_t ring_frames, bool reconnect) {
    (void)reconnect;
    if (sink.started) {
        return BTC_OK;
    }
    if (ring_frames < 2048) {
        ring_frames = 2048;
    }
    sink.buf = malloc(ring_frames * 2 * sizeof(int16_t));
    if (sink.buf == NULL) {
        return BTC_ERR_NOMEM;
    }
    btc_ring_init(&sink.ring, sink.buf, (uint32_t)ring_frames, 2);
    sink.volume = 127;  // unity, so a test reads back exactly what it sent
    sink.started = true;
    return BTC_OK;
}

static void stream_stop(void);

void btc_sink_stop(void) {
    if (!sink.started) {
        return;
    }
    stream_stop();
    btc_ring_init(&sink.ring, NULL, 0, 2);
    free(sink.buf);
    sink.buf = NULL;
    sink.started = false;
}

bool btc_sink_started(void) {
    return sink.started;
}

size_t btc_sink_read(int16_t *dst, size_t frames) {
    int32_t level = sink.volume;
    return btc_ring_read(&sink.ring, dst, frames, 2, level * level);
}

uint32_t btc_sink_changes(void) {
    LOCK();
    uint32_t c = sink.changes;
    sink.changes = 0;
    UNLOCK();
    return c;
}

void btc_sink_status(btc_sink_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->connected = sink.started;
    out->streaming = sink.streaming;
    if (out->connected) {
        memcpy(out->peer, phone, 6);
    }
    out->sample_rate = sink.rate;
    out->volume = sink.volume;
    out->play_status = sink.streaming ? BTC_PLAY_PLAYING : BTC_PLAY_STOPPED;
    out->track_number = out->track_count = out->battery = BTC_UNKNOWN;
    out->duration_ms = out->position_ms = BTC_UNKNOWN;
    out->channels = 2;
}

void btc_sink_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&sink.ring, out);
}

size_t btc_sink_text(btc_sink_text_t which, char *dst, size_t size) {
    return copy_text(dst, size, which == BTC_SINK_PEER_NAME ? "loopback" : "");
}

size_t btc_sink_cover_size(void) {
    return 0;
}

size_t btc_sink_cover(uint8_t *dst, size_t size) {
    (void)dst;
    (void)size;
    return 0;
}

void btc_sink_levels(uint16_t *left, uint16_t *right) {
    btc_ring_levels(&sink.ring, left, right);
}

int btc_sink_key(uint8_t key, int state) {
    (void)key;
    (void)state;
    return sink.started ? BTC_OK : BTC_ERR_STATE;
}

int btc_sink_set_player(uint8_t attr, uint8_t value) {
    (void)attr;
    (void)value;
    return BTC_ERR_UNSUPPORTED;
}

int btc_sink_refresh(void) {
    return BTC_OK;
}

void btc_sink_set_volume(uint8_t volume) {
    sink.volume = volume > 127 ? 127 : volume;
    sink_mark(BTC_CHANGED_VOLUME);
}

int btc_sink_connect(const uint8_t *addr) {
    (void)addr;
    return BTC_OK;
}

int btc_sink_disconnect(void) {
    return BTC_OK;
}

// The phone sending: the loop of PCM, in packets of 128 frames (an SBC
// frame's worth, near enough), each written when the clock says it's due.
static void *stream_run(void *arg) {
    (void)arg;
    const uint32_t packet = 128;
    double ns_per_frame = 1e9 / (sink.rate * sink.speed);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t sent = 0;
    uint32_t at = 0;
    while (!sink.thread_stop) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) * 1e9 + (now.tv_nsec - start.tv_nsec);
        while ((double)sent * ns_per_frame <= elapsed) {
            uint32_t n = packet;
            if (at + n > sink.pcm_frames) {
                n = sink.pcm_frames - at;
            }
            btc_ring_write(&sink.ring, &sink.pcm[at * 2], n, 2);
            sent += n;
            at = (at + n) % sink.pcm_frames;
        }
        struct timespec nap = { 0, 1000000 };   // 1 ms
        nanosleep(&nap, NULL);
    }
    return NULL;
}

static void stream_stop(void) {
    if (sink.thread_live) {
        sink.thread_stop = true;
        pthread_join(sink.thread, NULL);
        sink.thread_live = false;
        free(sink.pcm);
        sink.pcm = NULL;
        sink.streaming = false;
        sink_mark(BTC_CHANGED_STREAM | BTC_CHANGED_PLAYING);
    }
}

// ---- the hands-free unit ----

#define MIC_FRAMES 131072  // 8 s at 16 kHz: room for a whole test, read once at its end

static struct {
    bool started;
    uint8_t channels;
    uint32_t ring_ms;
    btc_ring_t ring;
    int16_t *buf;
    uint32_t changes;
    volatile bool audio;
    volatile uint32_t rate;
    int speaker_gain;
    // What the board queued for the phone: one writer (the board), one
    // reader (sent()).
    int16_t mic[MIC_FRAMES];
    volatile uint32_t mic_w, mic_r;
} hf;

int btc_hf_start(uint32_t ring_ms, uint8_t channels) {
    if (hf.started) {
        return BTC_OK;
    }
    uint32_t frames = 16000 * ring_ms / 1000;
    hf.buf = malloc(frames * sizeof(int16_t));
    if (hf.buf == NULL) {
        return BTC_ERR_NOMEM;
    }
    btc_ring_init(&hf.ring, hf.buf, frames, 1);
    hf.channels = channels == 2 ? 2 : 1;
    hf.ring_ms = ring_ms;
    hf.speaker_gain = 15;   // unity
    hf.started = true;
    return BTC_OK;
}

static void call_stop(void);

void btc_hf_stop(void) {
    if (!hf.started) {
        return;
    }
    call_stop();
    hf.audio = false;
    btc_ring_init(&hf.ring, NULL, 0, 1);
    free(hf.buf);
    hf.buf = NULL;
    hf.started = false;
}

bool btc_hf_started(void) {
    return hf.started;
}

size_t btc_hf_read(int16_t *dst, size_t frames) {
    if (!hf.audio) {
        memset(dst, 0, frames * hf.channels * sizeof(int16_t));
        return 0;
    }
    int32_t level = hf.speaker_gain * 127 / 15;
    return btc_ring_read(&hf.ring, dst, frames, hf.channels, level * level);
}

size_t btc_hf_write(const int16_t *pcm, size_t frames) {
    if (!hf.started || !hf.audio) {
        return 0;
    }
    uint32_t w = hf.mic_w;
    uint32_t space = MIC_FRAMES - (w - __atomic_load_n(&hf.mic_r, __ATOMIC_ACQUIRE));
    size_t n = frames < space ? frames : space;
    for (size_t i = 0; i < n; i++) {
        hf.mic[(w + i) % MIC_FRAMES] = pcm[i];
    }
    __atomic_store_n(&hf.mic_w, w + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
}

uint32_t btc_hf_changes(void) {
    LOCK();
    uint32_t c = hf.changes;
    hf.changes = 0;
    UNLOCK();
    return c;
}

void btc_hf_status(btc_hf_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->connected = hf.started;
    if (out->connected) {
        memcpy(out->peer, phone, 6);
    }
    out->service = out->signal = out->roaming = out->battery = BTC_UNKNOWN;
    out->call = hf.audio ? BTC_CALL_ACTIVE : BTC_CALL_IDLE;
    out->speaker_gain = hf.speaker_gain;
    out->mic_gain = BTC_UNKNOWN;
    out->audio = hf.audio;
    out->sample_rate = hf.audio ? hf.rate : 0;
}

void btc_hf_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&hf.ring, out);
}

size_t btc_hf_text(btc_hf_text_t which, char *dst, size_t size) {
    (void)which;
    return copy_text(dst, size, "");
}

int btc_hf_answer(void) {
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_hangup(void) {
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_dial(const char *number) {
    (void)number;
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_dtmf(char digit) {
    (void)digit;
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_voice_recognition(bool on) {
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_set_speaker_gain(int gain) {
    hf.speaker_gain = gain;
    return BTC_OK;
}

int btc_hf_set_mic_gain(int gain) {
    (void)gain;
    return BTC_OK;
}

int btc_hf_connect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_disconnect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}

int btc_hf_connect(const uint8_t *addr) {
    (void)addr;
    return BTC_OK;
}

int btc_hf_disconnect(void) {
    return BTC_OK;
}

// ---- _btclassic_host: the phone's side ----

static mp_obj_t host_stream(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_pcm, ARG_rate, ARG_speed };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_pcm, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_rate, MP_ARG_INT, {.u_int = 44100} },
        { MP_QSTR_speed, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    stream_stop();
    if (args[ARG_pcm].u_obj == mp_const_none) {
        return mp_const_none;
    }
    if (!sink.started) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("make an AudioSink first"));
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(args[ARG_pcm].u_obj, &b, MP_BUFFER_READ);
    if (b.len < 4 || b.len % 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("pcm is whole stereo frames"));
    }
    sink.pcm = malloc(b.len);
    if (sink.pcm == NULL) {
        mp_raise_OSError(12);
    }
    memcpy(sink.pcm, b.buf, b.len);
    sink.pcm_frames = b.len / 4;
    sink.speed = args[ARG_speed].u_obj == mp_const_none ? 1.0 : mp_obj_get_float(args[ARG_speed].u_obj);
    bool new_rate = sink.rate != (uint32_t)args[ARG_rate].u_int;
    sink.rate = (uint32_t)args[ARG_rate].u_int;
    btc_ring_restart(&sink.ring);
    sink.streaming = true;
    sink.thread_stop = false;
    if (pthread_create(&sink.thread, NULL, stream_run, NULL) != 0) {
        free(sink.pcm);
        sink.pcm = NULL;
        sink.streaming = false;
        mp_raise_OSError(11);
    }
    sink.thread_live = true;
    sink_mark(BTC_CHANGED_STREAM | BTC_CHANGED_PLAYING | (new_rate ? BTC_CHANGED_FORMAT : 0));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(host_stream_obj, 1, host_stream);

// sent(buf) -> bytes: what the board has queued for the phone, taken.
static mp_obj_t host_sent(mp_obj_t buf_in) {
    mp_buffer_info_t b;
    mp_get_buffer_raise(buf_in, &b, MP_BUFFER_WRITE);
    int16_t *out = b.buf;
    uint32_t r = hf.mic_r;
    uint32_t avail = __atomic_load_n(&hf.mic_w, __ATOMIC_ACQUIRE) - r;
    uint32_t n = b.len / 2 < avail ? b.len / 2 : avail;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = hf.mic[(r + i) % MIC_FRAMES];
    }
    __atomic_store_n(&hf.mic_r, r + n, __ATOMIC_RELEASE);
    return MP_OBJ_NEW_SMALL_INT(n * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_1(host_sent_obj, host_sent);

// The board's microphone: a capture ring the uplink pulls from, standing in
// for audioif's.
#define CAP_BYTES 65536
static uint8_t cap[CAP_BYTES];
static volatile uint32_t cap_w, cap_r;

static uint32_t cap_pull(uint8_t *dst, uint32_t want) {
    uint32_t r = cap_r;
    uint32_t avail = __atomic_load_n(&cap_w, __ATOMIC_ACQUIRE) - r;
    uint32_t n = want < avail ? want : avail;
    n -= n % 2;
    for (uint32_t i = 0; i < n; i++) {
        dst[i] = cap[(r + i) % CAP_BYTES];
    }
    __atomic_store_n(&cap_r, r + n, __ATOMIC_RELEASE);
    return n;
}

static void cap_write(const int16_t *pcm, uint32_t frames) {
    const uint8_t *b = (const uint8_t *)pcm;
    uint32_t w = cap_w;
    uint32_t space = CAP_BYTES - (w - __atomic_load_n(&cap_r, __ATOMIC_ACQUIRE));
    uint32_t n = frames * 2 < space ? frames * 2 : space;
    for (uint32_t i = 0; i < n; i++) {
        cap[(w + i) % CAP_BYTES] = b[i];
    }
    __atomic_store_n(&cap_w, w + n, __ATOMIC_RELEASE);
}

typedef uint32_t (*btc_mic_pull_t)(uint8_t *dst, uint32_t want);
extern btc_mic_pull_t btc_mic_pull_hook;

static struct {
    pthread_t thread;
    bool live;
    volatile bool stop;
    int16_t *voice, *mic;
    uint32_t voice_frames, mic_frames;
} call;

// The phone's voice and the board's microphone, 60 frames at a time (one
// mSBC packet's worth), each when the clock says it's due.
static void *call_run(void *arg) {
    (void)arg;
    const uint32_t packet = 60;
    double ns_per_frame = 1e9 / hf.rate;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint64_t sent = 0;
    uint32_t at_v = 0, at_m = 0;
    while (!call.stop) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - start.tv_sec) * 1e9 + (now.tv_nsec - start.tv_nsec);
        while ((double)sent * ns_per_frame <= elapsed) {
            for (uint32_t i = 0; i < packet; i++) {
                if (call.voice) {
                    btc_ring_write(&hf.ring, &call.voice[at_v], 1, 1);
                    at_v = (at_v + 1) % call.voice_frames;
                }
                if (call.mic) {
                    cap_write(&call.mic[at_m], 1);
                    at_m = (at_m + 1) % call.mic_frames;
                }
            }
            sent += packet;
        }
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    return NULL;
}

static void call_stop(void) {
    if (call.live) {
        call.stop = true;
        pthread_join(call.thread, NULL);
        call.live = false;
    }
    free(call.voice);
    free(call.mic);
    call.voice = call.mic = NULL;
}

static int16_t *pcm_copy(mp_obj_t obj, uint32_t *frames) {
    if (obj == mp_const_none) {
        return NULL;
    }
    mp_buffer_info_t b;
    mp_get_buffer_raise(obj, &b, MP_BUFFER_READ);
    if (b.len < 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("pcm is 16-bit mono"));
    }
    int16_t *p = malloc(b.len);
    if (p == NULL) {
        mp_raise_OSError(12);
    }
    memcpy(p, b.buf, b.len);
    *frames = b.len / 2;
    return p;
}

// call(rate, voice=None, mic=None): call audio up at `rate`, or down at 0.
static mp_obj_t host_call(size_t n_args, const mp_obj_t *args) {
    if (!hf.started) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("make a HandsFree first"));
    }
    mp_int_t rate = mp_obj_get_int(args[0]);
    if (rate != 0 && rate != 8000 && rate != 16000) {
        mp_raise_ValueError(MP_ERROR_TEXT("rate is 8000, 16000 or 0"));
    }
    call_stop();
    if (rate) {
        call.voice = pcm_copy(n_args > 1 ? args[1] : mp_const_none, &call.voice_frames);
        call.mic = pcm_copy(n_args > 2 ? args[2] : mp_const_none, &call.mic_frames);
        hf.rate = (uint32_t)rate;
        btc_ring_set_limit(&hf.ring, (uint32_t)rate * hf.ring_ms / 1000);
        hf.mic_r = hf.mic_w;
        cap_r = cap_w;
        #if BTCLASSIC_AUDIOSAMPLE
        btc_mic_pull_hook = cap_pull;
        #endif
        hf.audio = true;
        call.stop = false;
        if (pthread_create(&call.thread, NULL, call_run, NULL) != 0) {
            hf.audio = false;
            mp_raise_OSError(11);
        }
        call.live = true;
    } else {
        hf.audio = false;
        btc_ring_restart(&hf.ring);
    }
    LOCK();
    hf.changes |= BTC_CHANGED_AUDIO;
    UNLOCK();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(host_call_obj, 1, 3, host_call);

static const mp_rom_map_elem_t host_module_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__btclassic_host) },
    { MP_ROM_QSTR(MP_QSTR_stream), MP_ROM_PTR(&host_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_call), MP_ROM_PTR(&host_call_obj) },
    { MP_ROM_QSTR(MP_QSTR_sent), MP_ROM_PTR(&host_sent_obj) },
};
static MP_DEFINE_CONST_DICT(host_module_globals, host_module_table);

const mp_obj_module_t btclassic_host_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&host_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__btclassic_host, btclassic_host_module);
