// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The hands-free unit on Bluedroid's HFP client: the phone's indicators,
// its calls, and the call's audio both ways over HCI, which Bluedroid codes
// as CVSD (8 kHz) or wideband mSBC (16 kHz). See include/btclassic.h.
//
// The phone's voice goes into a ring of its own, read at the program's
// output pace like the sink's music. The microphone goes out of a second,
// smaller queue, written by callers and read by Bluedroid's task.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "btc_bluedroid.h"

#if BTC_HFP

#include "esp_hf_client_api.h"
#if BTC_CALL_AUDIO
#include "esp_hf_client_legacy_api.h"
#endif

static const char *TAG = "btclassic";

#define MAX_RATE 16000              // mSBC; CVSD is 8000
#define MIC_FRAMES 8192             // the microphone's queue: 0.5 s at 16 kHz
#define MIC_START_MS 120            // queued before the microphone goes out

static struct {
    bool started;
    uint8_t channels;
    uint32_t ring_ms;
    btc_ring_t ring;
    int16_t *ring_buf;
    uint32_t changes;

    bool connected;
    btc_addr_t peer;
    int service, signal, roaming, battery;
    int ind_call, ind_setup, held;
    bool waiting, ringing, voice_recognition, in_band_ring;
    int speaker_gain, mic_gain;
    char text[BTC_HF_TEXT_COUNT][BTC_TEXT_MAX];

    bool audio;
    uint32_t sample_rate;
    int16_t *mic;
    volatile uint32_t mic_w, mic_r;
    bool mic_running;
} h;

static void mark(uint32_t bits) {
    BTC_LOCK();
    h.changes |= bits;
    BTC_UNLOCK();
}

static void reset_phone(void) {
    BTC_LOCK();
    h.connected = false;
    h.service = h.signal = h.roaming = h.battery = BTC_UNKNOWN;
    h.ind_call = h.ind_setup = h.held = 0;
    h.waiting = h.ringing = h.voice_recognition = h.in_band_ring = false;
    h.speaker_gain = h.mic_gain = BTC_UNKNOWN;
    h.text[BTC_HF_OPERATOR][0] = 0;
    h.text[BTC_HF_SUBSCRIBER][0] = 0;
    BTC_UNLOCK();
}

static void set_phone(int *field, int value) {
    BTC_LOCK();
    bool changed = *field != value;
    *field = value;
    BTC_UNLOCK();
    if (changed) {
        mark(BTC_CHANGED_PHONE);
    }
}

static void set_cstr(btc_hf_text_t which, const char *src) {
    btc_set_text(h.text[which], (const uint8_t *)(src ? src : ""), src ? strlen(src) : 0);
}

// A call indicator moved: ask for the call list, which carries the number.
static void call_changed(void) {
    BTC_LOCK();
    if (h.ind_call == 0 && h.ind_setup == 0) {
        h.ringing = false;
        h.waiting = false;
        h.held = 0;
    }
    BTC_UNLOCK();
    mark(BTC_CHANGED_CALL);
    esp_hf_client_query_current_calls();
}

// ---- call audio ----

#if BTC_CALL_AUDIO
// The phone's voice: 16-bit mono at the call's rate. Each packet in is also
// the cue to send one out, so the microphone keeps the phone's pace.
static void sco_in(const uint8_t *buf, uint32_t len) {
    btc_ring_write(&h.ring, (const int16_t *)buf, len / 2, 1);
    esp_hf_client_outgoing_data_ready();
}

// The microphone, as much as Bluedroid asks for. It waits until a little is
// queued, and sends silence while there's too little, so the phone hears
// whole stretches rather than every gap in the writer.
static uint32_t sco_out(uint8_t *buf, uint32_t len) {
    int16_t *out = (int16_t *)buf;
    uint32_t want = len / 2;
    uint32_t r = h.mic_r;
    uint32_t avail = __atomic_load_n(&h.mic_w, __ATOMIC_ACQUIRE) - r;
    uint32_t n = 0;
    if (!h.mic_running && avail >= h.sample_rate * MIC_START_MS / 1000) {
        h.mic_running = true;
    }
    if (h.mic_running) {
        n = avail < want ? avail : want;
        for (uint32_t i = 0; i < n; i++) {
            out[i] = h.mic[(r + i) % MIC_FRAMES];
        }
        __atomic_store_n(&h.mic_r, r + n, __ATOMIC_RELEASE);
        if (n < want) {
            h.mic_running = false;
        }
    }
    memset(&out[n], 0, (want - n) * 2);
    return want * 2;
}

static void audio_start(uint32_t rate) {
    BTC_LOCK();
    h.audio = true;
    h.sample_rate = rate;
    BTC_UNLOCK();
    btc_ring_set_limit(&h.ring, rate * h.ring_ms / 1000);
    __atomic_store_n(&h.mic_r, __atomic_load_n(&h.mic_w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    h.mic_running = false;
    esp_hf_client_register_data_callback(sco_in, sco_out);
    mark(BTC_CHANGED_AUDIO);
    ESP_LOGI(TAG, "call audio: %u Hz", (unsigned)rate);
}
#endif

static void audio_stop(void) {
    if (!h.audio) {
        return;
    }
    BTC_LOCK();
    h.audio = false;
    h.sample_rate = 0;
    BTC_UNLOCK();
    btc_ring_restart(&h.ring);
    mark(BTC_CHANGED_AUDIO);
}

// ---- the phone's events ----

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param) {
    switch (event) {
        case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
                BTC_LOCK();
                h.connected = true;
                memcpy(h.peer, param->conn_stat.remote_bda, sizeof(btc_addr_t));
                BTC_UNLOCK();
                esp_hf_client_query_current_operator_name();
                esp_hf_client_retrieve_subscriber_info();
                esp_hf_client_query_current_calls();
                mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_PHONE);
            } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
                audio_stop();
                bool was = h.connected;
                reset_phone();
                mark(BTC_CHANGED_PHONE | BTC_CHANGED_CALL | (was ? BTC_CHANGED_CONNECTION : 0));
            }
            break;
        case ESP_HF_CLIENT_AUDIO_STATE_EVT:
            #if BTC_CALL_AUDIO
            if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
                audio_start(8000);      // CVSD
            } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
                audio_start(16000);     // mSBC, wideband
            } else if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
                audio_stop();
            }
            #else
            // No call audio in this build: hand the call back to the phone.
            if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED
                || param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
                esp_hf_client_disconnect_audio(param->audio_stat.remote_bda);
            }
            #endif
            break;
        case ESP_HF_CLIENT_BVRA_EVT:
            BTC_LOCK();
            h.voice_recognition = param->bvra.value == ESP_HF_VR_STATE_ENABLED;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_CIND_CALL_EVT:
            h.ind_call = param->call.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
            h.ind_setup = param->call_setup.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
            h.held = param->call_held.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
            set_phone(&h.service, param->service_availability.status);
            break;
        case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
            set_phone(&h.signal, param->signal_strength.value);
            break;
        case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
            set_phone(&h.roaming, param->roaming.status);
            break;
        case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
            set_phone(&h.battery, param->battery_level.value);
            break;
        case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
            set_cstr(BTC_HF_OPERATOR, param->cops.name);
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_CLIP_EVT:
            set_cstr(BTC_HF_CALLER, param->clip.number);
            mark(BTC_CHANGED_CALL);
            break;
        case ESP_HF_CLIENT_CCWA_EVT:
            BTC_LOCK();
            h.waiting = true;
            BTC_UNLOCK();
            mark(BTC_CHANGED_CALL);
            break;
        case ESP_HF_CLIENT_CLCC_EVT:
            if (param->clcc.number && param->clcc.number[0]) {
                set_cstr(BTC_HF_CALLER, param->clcc.number);
                mark(BTC_CHANGED_CALL);
            }
            break;
        case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
            if (param->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) {
                set_phone(&h.speaker_gain, param->volume_control.volume);
            } else {
                set_phone(&h.mic_gain, param->volume_control.volume);
            }
            break;
        case ESP_HF_CLIENT_CNUM_EVT:
            set_cstr(BTC_HF_SUBSCRIBER, param->cnum.number);
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_BSIR_EVT:
            BTC_LOCK();
            h.in_band_ring = param->bsir.state == ESP_HF_CLIENT_IN_BAND_RINGTONE_PROVIDED;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_RING_IND_EVT:
            BTC_LOCK();
            h.ringing = true;
            BTC_UNLOCK();
            mark(BTC_CHANGED_CALL);
            break;
        default:
            break;
    }
}

// ---- start and stop ----

int btc_hf_start(uint32_t ring_ms, uint8_t channels) {
    // Bluedroid runs one HFP role at a time.
    if (h.started || !btc_radio_active() || btc_ag_started()) {
        return BTC_ERR_STATE;
    }
    uint32_t frames = MAX_RATE * ring_ms / 1000;
    int16_t *buf = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    int16_t *mic = NULL;
    #if BTC_CALL_AUDIO
    mic = heap_caps_malloc(MIC_FRAMES * sizeof(int16_t), MALLOC_CAP_8BIT);
    #endif
    if (buf == NULL || (BTC_CALL_AUDIO && mic == NULL)) {
        heap_caps_free(buf);
        heap_caps_free(mic);
        return BTC_ERR_NOMEM;
    }
    memset(&h, 0, sizeof(h));
    h.ring_buf = buf;
    h.mic = mic;
    h.ring_ms = ring_ms;
    h.channels = channels == 2 ? 2 : 1;
    btc_ring_init(&h.ring, buf, frames, 1);
    reset_phone();
    esp_hf_client_register_callback(hf_cb);
    esp_err_t err = esp_hf_client_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hands-free didn't start: %s", esp_err_to_name(err));
        btc_ring_init(&h.ring, NULL, 0, 1);
        heap_caps_free(h.ring_buf);
        heap_caps_free(h.mic);
        h.ring_buf = h.mic = NULL;
        return btc_err(err);
    }
    h.started = true;
    btc_radio_profile(BTC_PROFILE_HF, true, 0);
    // Follow a sender that's already here.
    btc_addr_t peer;
    if (btc_sink_peer(peer)) {
        esp_hf_client_connect(peer);
    }
    return BTC_OK;
}

void btc_hf_stop(void) {
    if (!h.started) {
        return;
    }
    if (h.connected) {
        btc_hf_disconnect();
        for (int i = 0; i < 100 && h.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    h.started = false;
    esp_hf_client_deinit();
    btc_radio_profile(BTC_PROFILE_HF, false, 0);
    // Bluedroid's task may still be finishing a callback that writes the ring.
    vTaskDelay(pdMS_TO_TICKS(50));
    h.audio = false;
    btc_ring_init(&h.ring, NULL, 0, 1);
    heap_caps_free(h.ring_buf);
    heap_caps_free(h.mic);
    h.ring_buf = h.mic = NULL;
}

bool btc_hf_started(void) {
    return h.started;
}

void btc_hf_follow(const uint8_t *bda) {
    if (h.started && !h.connected) {
        esp_bd_addr_t a;
        memcpy(a, bda, sizeof(a));
        esp_hf_client_connect(a);
    }
}

// ---- audio ----

size_t btc_hf_read(int16_t *dst, size_t frames) {
    BTC_LOCK();
    bool audio = h.audio;
    int gain = h.speaker_gain;
    BTC_UNLOCK();
    if (!audio) {
        memset(dst, 0, frames * h.channels * sizeof(int16_t));
        return 0;
    }
    // The call follows the phone's call volume.
    int32_t level = gain >= 0 ? gain * 127 / 15 : 127;
    return btc_ring_read(&h.ring, dst, frames, h.channels ? h.channels : 1, level * level);
}

size_t btc_hf_write(const int16_t *pcm, size_t frames) {
    #if BTC_CALL_AUDIO
    if (!h.started || !h.audio || h.mic == NULL) {
        return 0;
    }
    uint32_t w = h.mic_w;
    uint32_t used = w - __atomic_load_n(&h.mic_r, __ATOMIC_ACQUIRE);
    uint32_t space = MIC_FRAMES - used;
    size_t n = frames < space ? frames : space;
    for (size_t i = 0; i < n; i++) {
        h.mic[(w + i) % MIC_FRAMES] = pcm[i];
    }
    __atomic_store_n(&h.mic_w, w + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
    #else
    (void)pcm;
    (void)frames;
    return 0;
    #endif
}

// ---- state ----

uint32_t btc_hf_changes(void) {
    BTC_LOCK();
    uint32_t c = h.changes;
    h.changes = 0;
    BTC_UNLOCK();
    return c;
}

static int call_state(void) {
    if (h.ind_setup == 1) {
        return BTC_CALL_INCOMING;
    }
    if (h.ind_setup == 2) {
        return BTC_CALL_DIALING;
    }
    if (h.ind_setup == 3) {
        return BTC_CALL_ALERTING;
    }
    if (h.ind_call) {
        return h.held == 2 ? BTC_CALL_HELD : BTC_CALL_ACTIVE;
    }
    return BTC_CALL_IDLE;
}

void btc_hf_status(btc_hf_status_t *out) {
    BTC_LOCK();
    out->connected = h.connected;
    memcpy(out->peer, h.peer, sizeof(btc_addr_t));
    out->service = h.service;
    out->signal = h.signal;
    out->roaming = h.roaming;
    out->battery = h.battery;
    out->call = call_state();
    out->held = h.held;
    out->waiting = h.waiting;
    out->ringing = h.ringing;
    out->voice_recognition = h.voice_recognition;
    out->in_band_ring = h.in_band_ring;
    out->speaker_gain = h.speaker_gain;
    out->mic_gain = h.mic_gain;
    out->audio = h.audio;
    out->sample_rate = h.sample_rate;
    BTC_UNLOCK();
}

void btc_hf_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&h.ring, out);
}

size_t btc_hf_text(btc_hf_text_t which, char *dst, size_t size) {
    if (which >= BTC_HF_TEXT_COUNT) {
        return 0;
    }
    return btc_get_text(h.text[which], dst, size);
}

// ---- commands to the phone ----

static int ready(void) {
    return h.started && h.connected ? BTC_OK : BTC_ERR_STATE;
}

int btc_hf_answer(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_answer_call());
}

int btc_hf_hangup(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_reject_call());
}

int btc_hf_dial(const char *number) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_dial(number));
}

int btc_hf_dtmf(char digit) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_send_dtmf(digit));
}

int btc_hf_voice_recognition(bool on) {
    int err = ready();
    if (err) {
        return err;
    }
    return btc_err(on ? esp_hf_client_start_voice_recognition() : esp_hf_client_stop_voice_recognition());
}

static int set_gain(esp_hf_volume_control_target_t target, int *field, int gain) {
    int err = ready();
    if (err) {
        return err;
    }
    if (gain < 0 || gain > 15) {
        return BTC_ERR_ARG;
    }
    err = btc_err(esp_hf_client_volume_update(target, gain));
    if (err == BTC_OK) {
        set_phone(field, gain);
    }
    return err;
}

int btc_hf_set_speaker_gain(int gain) {
    return set_gain(ESP_HF_VOLUME_CONTROL_TARGET_SPK, &h.speaker_gain, gain);
}

int btc_hf_set_mic_gain(int gain) {
    return set_gain(ESP_HF_VOLUME_CONTROL_TARGET_MIC, &h.mic_gain, gain);
}

int btc_hf_connect_audio(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_connect_audio(h.peer));
}

int btc_hf_disconnect_audio(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_disconnect_audio(h.peer));
}

int btc_hf_connect(const uint8_t *addr) {
    if (!h.started) {
        return BTC_ERR_STATE;
    }
    btc_addr_t a;
    if (addr) {
        memcpy(a, addr, sizeof(a));
    } else if (!btc_sink_peer(a)) {
        return BTC_ERR_STATE;
    }
    return btc_err(esp_hf_client_connect(a));
}

int btc_hf_disconnect(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_client_disconnect(h.peer));
}

#else   // no HFP in this build

int btc_hf_start(uint32_t ring_ms, uint8_t channels) {
    (void)ring_ms;
    (void)channels;
    return BTC_ERR_UNSUPPORTED;
}
void btc_hf_stop(void) {
}
bool btc_hf_started(void) {
    return false;
}
void btc_hf_follow(const uint8_t *bda) {
    (void)bda;
}
size_t btc_hf_read(int16_t *dst, size_t frames) {
    memset(dst, 0, frames * sizeof(int16_t));
    return 0;
}
size_t btc_hf_write(const int16_t *pcm, size_t frames) {
    (void)pcm;
    (void)frames;
    return 0;
}
uint32_t btc_hf_changes(void) {
    return 0;
}
void btc_hf_status(btc_hf_status_t *out) {
    memset(out, 0, sizeof(*out));
}
void btc_hf_stats(btc_ring_stats_t *out) {
    memset(out, 0, sizeof(*out));
}
size_t btc_hf_text(btc_hf_text_t which, char *dst, size_t size) {
    (void)which;
    if (size) {
        dst[0] = 0;
    }
    return 0;
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
    (void)gain;
    return BTC_ERR_UNSUPPORTED;
}
int btc_hf_set_mic_gain(int gain) {
    (void)gain;
    return BTC_ERR_UNSUPPORTED;
}
int btc_hf_connect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hf_disconnect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hf_connect(const uint8_t *addr) {
    (void)addr;
    return BTC_ERR_UNSUPPORTED;
}
int btc_hf_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}

#endif
