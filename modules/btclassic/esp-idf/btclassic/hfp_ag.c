// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio gateway on Bluedroid's HFP AG: the board is the phone, and a
// headset, speaker or car kit is the hands-free unit. See include/btclassic.h.
//
// Calls go through Bluedroid's phone state machine (the answer, reject,
// out_call and end_call calls all feed it the new counts and states), which
// sends the headset its RING, +CLIP and +CIEV indicators and opens the
// call's audio where a phone would.
//
// Call audio runs over HCI, coded by Bluedroid as CVSD (8 kHz) or mSBC
// (16 kHz). The headset's microphone lands in a ring read at the program's
// pace. What the board says goes out of a queue, paced by the link itself:
// each packet that comes in earns one going out, so the two directions keep
// the controller's clock. If none comes in for a while, a timer stands in.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "btc_bluedroid.h"

#if BTC_AG

#include "esp_hf_ag_api.h"
#if BTC_AG_AUDIO
#include "esp_hf_ag_legacy_api.h"
#endif

static const char *TAG = "btclassic";

#define MAX_RATE 16000
#define SEND_START_MS 60        // queued before the board's voice goes out
#define CREDIT_MAX 960          // bytes of PCM the link may owe: 2 mSBC frames
#define TICK_US 15000           // the stand-in clock's period
#define QUIET_US 50000          // no packet in for this long: the timer paces
#define DTMF_MAX 32

static struct {
    bool started;
    uint32_t ring_ms;
    btc_ring_t ring;            // the headset's microphone
    int16_t *ring_buf;
    uint32_t changes;

    bool connected;
    btc_addr_t peer;
    btc_addr_t last;            // the last headset, for connect(None)
    bool has_last;
    uint32_t peer_features;
    int service, signal, roaming, battery;
    int call;                   // btc_call_t
    bool outgoing;              // the call was dialed, not rung
    bool in_band_ring, voice_recognition, nrec, wideband;
    int speaker_gain, mic_gain;
    char text[BTC_AG_TEXT_COUNT][BTC_TEXT_MAX];
    char dtmf[DTMF_MAX];
    uint8_t dtmf_len;

    // The call's audio.
    bool audio;
    uint32_t sample_rate;
    int16_t *out;               // what the board says, for the headset
    uint32_t out_frames;
    volatile uint32_t out_w, out_r;
    bool out_running;
    volatile int32_t credit;    // bytes of PCM the link has asked for
    volatile int64_t last_in_us;
    int64_t tick_us;
    esp_timer_handle_t timer;
    volatile uint32_t sent, silent;
} a;

static void mark(uint32_t bits) {
    BTC_LOCK();
    a.changes |= bits;
    BTC_UNLOCK();
}

static void set_int(int *field, int value, uint32_t bits) {
    BTC_LOCK();
    bool changed = *field != value;
    *field = value;
    BTC_UNLOCK();
    if (changed) {
        mark(bits);
    }
}

static void set_cstr(btc_ag_text_t which, const char *src) {
    btc_set_text(a.text[which], (const uint8_t *)(src ? src : ""), src ? strlen(src) : 0);
}

static esp_hf_call_addr_type_t number_type(const char *number) {
    return number && number[0] == '+' ? ESP_HF_CALL_ADDR_TYPE_INTERNATIONAL : ESP_HF_CALL_ADDR_TYPE_UNKNOWN;
}

// ---- call audio ----

#if BTC_AG_AUDIO

// The headset's microphone, 16-bit mono. Each packet in is the cue to send
// one out.
static void sco_in(const uint8_t *buf, uint32_t len) {
    btc_ring_write(&a.ring, (const int16_t *)buf, len / 2, 1);
    a.last_in_us = esp_timer_get_time();
    int32_t c = a.credit + (int32_t)len;
    a.credit = c > CREDIT_MAX ? CREDIT_MAX : c;
    esp_hf_ag_outgoing_data_ready();
}

// What Bluedroid sends next: a whole packet's worth or nothing, so it asks
// again at the next cue rather than flooding the link.
static uint32_t sco_out(uint8_t *buf, uint32_t len) {
    if (a.credit < (int32_t)len || a.out == NULL) {
        return 0;
    }
    a.credit -= len;
    int16_t *dst = (int16_t *)buf;
    uint32_t want = len / 2;
    uint32_t r = a.out_r;
    uint32_t avail = __atomic_load_n(&a.out_w, __ATOMIC_ACQUIRE) - r;
    uint32_t n = 0;
    if (!a.out_running && avail >= a.sample_rate * SEND_START_MS / 1000) {
        a.out_running = true;
    }
    if (a.out_running) {
        n = avail < want ? avail : want;
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = a.out[(r + i) % a.out_frames];
        }
        __atomic_store_n(&a.out_r, r + n, __ATOMIC_RELEASE);
        if (n < want) {
            a.out_running = false;
        }
    }
    memset(&dst[n], 0, (want - n) * 2);
    a.sent += want;
    a.silent += want - n;
    return len;
}

// The stand-in clock: while no packet comes in (a headset that sends
// nothing until it hears something), earn credit by the time gone by.
static void tick(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    int64_t since = now - a.tick_us;
    a.tick_us = now;
    if (!a.audio || now - a.last_in_us < QUIET_US) {
        return;
    }
    int32_t c = a.credit + (int32_t)(since * a.sample_rate / 1000000 * 2);
    a.credit = c > CREDIT_MAX ? CREDIT_MAX : c;
    esp_hf_ag_outgoing_data_ready();
}

static void audio_start(uint32_t rate) {
    BTC_LOCK();
    a.audio = true;
    a.sample_rate = rate;
    BTC_UNLOCK();
    btc_ring_set_limit(&a.ring, rate * a.ring_ms / 1000);
    __atomic_store_n(&a.out_r, __atomic_load_n(&a.out_w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    a.out_running = false;
    a.credit = 0;
    a.last_in_us = 0;
    a.tick_us = esp_timer_get_time();
    esp_hf_ag_register_data_callback(sco_in, sco_out);
    if (a.timer) {
        esp_timer_start_periodic(a.timer, TICK_US);
    }
    mark(BTC_CHANGED_AUDIO);
    ESP_LOGI(TAG, "gateway call audio: %u Hz", (unsigned)rate);
}
#endif

static void audio_stop(void) {
    if (!a.audio) {
        return;
    }
    #if BTC_AG_AUDIO
    if (a.timer) {
        esp_timer_stop(a.timer);
    }
    #endif
    BTC_LOCK();
    a.audio = false;
    a.sample_rate = 0;
    BTC_UNLOCK();
    btc_ring_restart(&a.ring);
    mark(BTC_CHANGED_AUDIO);
}

// ---- calls: the phone state machine ----

static void set_call(int call) {
    set_int(&a.call, call, BTC_CHANGED_CALL);
}

// Tell Bluedroid the new state, through the call that fits it.
typedef esp_err_t (*state_fn_t)(esp_bd_addr_t, int, int, esp_hf_call_status_t,
    esp_hf_call_setup_status_t, char *, esp_hf_call_addr_type_t);

static int phone_state(state_fn_t fn, int active, esp_hf_call_setup_status_t setup, int call) {
    char number[BTC_TEXT_MAX];
    btc_get_text(a.text[BTC_AG_CALLER], number, sizeof(number));
    esp_err_t err = fn(a.peer, active, 0,
        active ? ESP_HF_CALL_STATUS_CALL_IN_PROGRESS : ESP_HF_CALL_STATUS_NO_CALLS,
        setup, number[0] ? number : NULL, number_type(number));
    if (err == ESP_OK) {
        set_call(call);
    }
    return btc_err(err);
}

static int do_answer(void) {
    if (a.call == BTC_CALL_INCOMING) {
        return phone_state(esp_hf_ag_answer_call, 1, ESP_HF_CALL_SETUP_STATUS_IDLE, BTC_CALL_ACTIVE);
    }
    if (a.call == BTC_CALL_DIALING || a.call == BTC_CALL_ALERTING) {
        return phone_state(esp_hf_ag_answer_call, 1, ESP_HF_CALL_SETUP_STATUS_IDLE, BTC_CALL_ACTIVE);
    }
    return BTC_ERR_STATE;
}

static int do_hangup(void) {
    if (a.call == BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    if (a.call == BTC_CALL_INCOMING) {
        return phone_state(esp_hf_ag_reject_call, 0, ESP_HF_CALL_SETUP_STATUS_IDLE, BTC_CALL_IDLE);
    }
    return phone_state(esp_hf_ag_end_call, 0, ESP_HF_CALL_SETUP_STATUS_IDLE, BTC_CALL_IDLE);
}

static int do_dial(const char *number) {
    if (a.call != BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    set_cstr(BTC_AG_CALLER, number);
    a.outgoing = true;
    return phone_state(esp_hf_ag_out_call, 0, ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING, BTC_CALL_DIALING);
}

// ---- the headset's events ----

static void clcc(esp_bd_addr_t addr) {
    if (a.call != BTC_CALL_IDLE) {
        char number[BTC_TEXT_MAX];
        btc_get_text(a.text[BTC_AG_CALLER], number, sizeof(number));
        esp_hf_current_call_status_t st =
            a.call == BTC_CALL_INCOMING ? ESP_HF_CURRENT_CALL_STATUS_INCOMING
            : a.call == BTC_CALL_DIALING ? ESP_HF_CURRENT_CALL_STATUS_DIALING
            : a.call == BTC_CALL_ALERTING ? ESP_HF_CURRENT_CALL_STATUS_ALERTING
            : a.call == BTC_CALL_HELD ? ESP_HF_CURRENT_CALL_STATUS_HELD
            : ESP_HF_CURRENT_CALL_STATUS_ACTIVE;
        esp_hf_ag_clcc_response(addr, 1,
            a.outgoing ? ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING : ESP_HF_CURRENT_CALL_DIRECTION_INCOMING,
            st, ESP_HF_CURRENT_CALL_MODE_VOICE, ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE,
            number, number_type(number));
    }
    // Index 0 ends the list with OK.
    esp_hf_ag_clcc_response(addr, 0, 0, 0, 0, 0, NULL, 0);
}

static void ag_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param) {
    switch (event) {
        case ESP_HF_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED) {
                BTC_LOCK();
                a.connected = true;
                memcpy(a.peer, param->conn_stat.remote_bda, sizeof(btc_addr_t));
                memcpy(a.last, a.peer, sizeof(btc_addr_t));
                a.has_last = true;
                a.peer_features = param->conn_stat.peer_feat;
                BTC_UNLOCK();
                ESP_LOGI(TAG, "gateway: headset connected, features 0x%x", (unsigned)a.peer_features);
                if (a.in_band_ring) {
                    esp_hf_ag_bsir(a.peer, ESP_HF_IN_BAND_RINGTONE_PROVIDED);
                }
                mark(BTC_CHANGED_CONNECTION);
            } else if (param->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
                audio_stop();
                bool was = a.connected;
                BTC_LOCK();
                a.connected = false;
                a.speaker_gain = a.mic_gain = BTC_UNKNOWN;
                a.voice_recognition = false;
                BTC_UNLOCK();
                if (was) {
                    mark(BTC_CHANGED_CONNECTION);
                }
            }
            break;
        case ESP_HF_AUDIO_STATE_EVT:
            #if BTC_AG_AUDIO
            if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED) {
                audio_start(8000);
            } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                audio_start(16000);
            } else if (param->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
                audio_stop();
            }
            #endif
            break;
        case ESP_HF_BVRA_RESPONSE_EVT:
            BTC_LOCK();
            a.voice_recognition = param->vra_rep.value == ESP_HF_VR_STATE_ENABLED;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_VOLUME_CONTROL_EVT:
            if (param->volume_control.type == ESP_HF_VOLUME_TYPE_SPK) {
                set_int(&a.speaker_gain, param->volume_control.volume, BTC_CHANGED_PHONE);
            } else {
                set_int(&a.mic_gain, param->volume_control.volume, BTC_CHANGED_PHONE);
            }
            break;
        case ESP_HF_UNAT_RESPONSE_EVT:
            // A command the gateway doesn't know: refuse it.
            esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, NULL);
            break;
        case ESP_HF_IND_UPDATE_EVT:
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SERVICE, a.service);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SIGNAL, a.signal);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_ROAM, a.roaming);
            esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_BATTCHG, a.battery);
            break;
        case ESP_HF_CIND_RESPONSE_EVT: {
            int c = a.call;
            esp_hf_ag_cind_response(param->cind_rep.remote_addr,
                (c == BTC_CALL_ACTIVE || c == BTC_CALL_HELD) ? ESP_HF_CALL_STATUS_CALL_IN_PROGRESS : ESP_HF_CALL_STATUS_NO_CALLS,
                c == BTC_CALL_INCOMING ? ESP_HF_CALL_SETUP_STATUS_INCOMING
                : c == BTC_CALL_DIALING ? ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING
                : c == BTC_CALL_ALERTING ? ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING
                : ESP_HF_CALL_SETUP_STATUS_IDLE,
                a.service, a.signal, a.roaming, a.battery,
                c == BTC_CALL_HELD ? ESP_HF_CALL_HELD_STATUS_HELD : ESP_HF_CALL_HELD_STATUS_NONE);
            break;
        }
        case ESP_HF_COPS_RESPONSE_EVT: {
            char name[BTC_TEXT_MAX];
            btc_get_text(a.text[BTC_AG_OPERATOR], name, sizeof(name));
            esp_hf_ag_cops_response(param->cops_rep.remote_addr, name);
            break;
        }
        case ESP_HF_CLCC_RESPONSE_EVT:
            clcc(param->clcc_rep.remote_addr);
            break;
        case ESP_HF_CNUM_RESPONSE_EVT: {
            char number[BTC_TEXT_MAX];
            btc_get_text(a.text[BTC_AG_NUMBER], number, sizeof(number));
            if (number[0]) {
                esp_hf_ag_cnum_response(param->cnum_rep.remote_addr, number,
                    number[0] == '+' ? 145 : 129, ESP_HF_SUBSCRIBER_SERVICE_TYPE_VOICE);
            } else {
                esp_hf_ag_cmee_send(param->cnum_rep.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            }
            break;
        }
        case ESP_HF_VTS_RESPONSE_EVT:
            if (param->vts_rep.code) {
                BTC_LOCK();
                for (const char *c = param->vts_rep.code; *c && a.dtmf_len < DTMF_MAX; c++) {
                    a.dtmf[a.dtmf_len++] = *c;
                }
                BTC_UNLOCK();
                mark(BTC_CHANGED_REQUEST);
            }
            break;
        case ESP_HF_NREC_RESPONSE_EVT:
            BTC_LOCK();
            a.nrec = param->nrec.state == ESP_HF_NREC_START;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PHONE);
            break;
        case ESP_HF_ATA_RESPONSE_EVT:
            // The headset's answer button.
            do_answer();
            break;
        case ESP_HF_CHUP_RESPONSE_EVT:
            // Its hang-up or reject.
            do_hangup();
            break;
        case ESP_HF_DIAL_EVT: {
            const char *number = NULL;
            char last[BTC_TEXT_MAX];
            if (param->out_call.type == ESP_HF_DIAL_NUM && param->out_call.num_or_loc) {
                number = param->out_call.num_or_loc;
            } else if (param->out_call.num_or_loc == NULL) {
                // Redial.
                btc_get_text(a.text[BTC_AG_DIALED], last, sizeof(last));
                number = last[0] ? last : NULL;
            }
            if (number == NULL || a.call != BTC_CALL_IDLE) {
                // Memory dialing isn't kept; nor is a second call.
                esp_hf_ag_cmee_send(param->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_ERR, ESP_HF_CME_AG_FAILURE);
                break;
            }
            set_cstr(BTC_AG_DIALED, number);
            esp_hf_ag_cmee_send(param->out_call.remote_addr, ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            do_dial(number);
            mark(BTC_CHANGED_REQUEST);
            break;
        }
        case ESP_HF_WBS_RESPONSE_EVT:
            BTC_LOCK();
            a.wideband = param->wbs_rep.codec == ESP_HF_WBS_YES;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PHONE);
            break;
        default:
            break;
    }
}

// ---- start and stop ----

int btc_ag_start(uint32_t ring_ms) {
    if (a.started || !btc_radio_active() || btc_hf_started()) {
        return BTC_ERR_STATE;
    }
    uint32_t frames = MAX_RATE * ring_ms / 1000;
    int16_t *buf = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    }
    int16_t *out = NULL;
    #if BTC_AG_AUDIO
    out = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_8BIT);
    #endif
    if (buf == NULL || (BTC_AG_AUDIO && out == NULL)) {
        heap_caps_free(buf);
        heap_caps_free(out);
        return BTC_ERR_NOMEM;
    }
    // Settings made before a start, and the last headset, carry over.
    btc_addr_t last;
    bool has_last = a.has_last;
    memcpy(last, a.last, sizeof(last));
    char op[BTC_TEXT_MAX], num[BTC_TEXT_MAX];
    strcpy(op, a.text[BTC_AG_OPERATOR][0] ? a.text[BTC_AG_OPERATOR] : "MicroPython");
    strcpy(num, a.text[BTC_AG_NUMBER]);
    memset(&a, 0, sizeof(a));
    memcpy(a.last, last, sizeof(last));
    a.has_last = has_last;
    strcpy(a.text[BTC_AG_OPERATOR], op);
    strcpy(a.text[BTC_AG_NUMBER], num);
    a.ring_buf = buf;
    a.out = out;
    a.out_frames = frames;
    a.ring_ms = ring_ms;
    a.service = 1;
    a.signal = 5;
    a.roaming = 0;
    a.battery = 5;
    a.call = BTC_CALL_IDLE;
    a.speaker_gain = a.mic_gain = BTC_UNKNOWN;
    btc_ring_init(&a.ring, buf, frames, 1);
    #if BTC_AG_AUDIO
    const esp_timer_create_args_t targs = {.callback = tick, .name = "btclassic_ag"};
    esp_timer_create(&targs, &a.timer);
    #endif
    esp_hf_ag_register_callback(ag_cb);
    esp_err_t err = esp_hf_ag_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gateway didn't start: %s", esp_err_to_name(err));
        if (a.timer) {
            esp_timer_delete(a.timer);
            a.timer = NULL;
        }
        btc_ring_init(&a.ring, NULL, 0, 1);
        heap_caps_free(a.ring_buf);
        heap_caps_free(a.out);
        a.ring_buf = a.out = NULL;
        return btc_err(err);
    }
    a.started = true;
    btc_radio_profile(BTC_PROFILE_AG, true, 0);
    return BTC_OK;
}

void btc_ag_stop(void) {
    if (!a.started) {
        return;
    }
    if (a.connected) {
        btc_ag_disconnect();
        for (int i = 0; i < 100 && a.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    a.started = false;
    if (a.timer) {
        esp_timer_stop(a.timer);
        esp_timer_delete(a.timer);
        a.timer = NULL;
    }
    esp_hf_ag_deinit();
    btc_radio_profile(BTC_PROFILE_AG, false, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    a.audio = false;
    btc_ring_init(&a.ring, NULL, 0, 1);
    heap_caps_free(a.ring_buf);
    heap_caps_free(a.out);
    a.ring_buf = a.out = NULL;
}

bool btc_ag_started(void) {
    return a.started;
}

static int ready(void) {
    return a.started && a.connected ? BTC_OK : BTC_ERR_STATE;
}

int btc_ag_connect(const uint8_t *addr) {
    if (!a.started) {
        return BTC_ERR_STATE;
    }
    esp_bd_addr_t bda;
    if (addr) {
        memcpy(bda, addr, sizeof(bda));
    } else if (a.has_last) {
        memcpy(bda, a.last, sizeof(bda));
    } else {
        return BTC_ERR_STATE;
    }
    return btc_err(esp_hf_ag_slc_connect(bda));
}

int btc_ag_disconnect(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_ag_slc_disconnect(a.peer));
}

int btc_ag_connect_audio(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_ag_audio_connect(a.peer));
}

int btc_ag_disconnect_audio(void) {
    int err = ready();
    return err ? err : btc_err(esp_hf_ag_audio_disconnect(a.peer));
}

// ---- what the gateway reports ----

static void report(esp_hf_ciev_report_type_t type, int *field, int value, int lo, int hi) {
    if (value == BTC_UNKNOWN) {
        return;
    }
    value = value < lo ? lo : value > hi ? hi : value;
    BTC_LOCK();
    bool changed = *field != value;
    *field = value;
    BTC_UNLOCK();
    if (changed && a.connected) {
        esp_hf_ag_ciev_report(a.peer, type, value);
    }
}

int btc_ag_set_indicators(int service, int signal, int roaming, int battery) {
    if (!a.started) {
        return BTC_ERR_STATE;
    }
    report(ESP_HF_IND_TYPE_SERVICE, &a.service, service, 0, 1);
    report(ESP_HF_IND_TYPE_SIGNAL, &a.signal, signal, 0, 5);
    report(ESP_HF_IND_TYPE_ROAM, &a.roaming, roaming, 0, 1);
    report(ESP_HF_IND_TYPE_BATTCHG, &a.battery, battery, 0, 5);
    return BTC_OK;
}

int btc_ag_set_text(btc_ag_text_t which, const char *text) {
    if (which != BTC_AG_OPERATOR && which != BTC_AG_NUMBER) {
        return BTC_ERR_ARG;
    }
    set_cstr(which, text);
    return BTC_OK;
}

// ---- calls ----

int btc_ag_ring(const char *number) {
    int err = ready();
    if (err) {
        return err;
    }
    if (a.call != BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    set_cstr(BTC_AG_CALLER, number);
    a.outgoing = false;
    // An incoming call: Bluedroid sends RING and +CLIP, and repeats them.
    return phone_state(esp_hf_ag_answer_call, 0, ESP_HF_CALL_SETUP_STATUS_INCOMING, BTC_CALL_INCOMING);
}

int btc_ag_dial(const char *number) {
    int err = ready();
    if (err) {
        return err;
    }
    if (number == NULL || !number[0]) {
        return BTC_ERR_ARG;
    }
    set_cstr(BTC_AG_DIALED, number);
    return do_dial(number);
}

int btc_ag_alerting(void) {
    int err = ready();
    if (err) {
        return err;
    }
    if (a.call != BTC_CALL_DIALING) {
        return BTC_ERR_STATE;
    }
    return phone_state(esp_hf_ag_out_call, 0, ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING, BTC_CALL_ALERTING);
}

int btc_ag_answer(void) {
    int err = ready();
    return err ? err : do_answer();
}

int btc_ag_hangup(void) {
    int err = ready();
    return err ? err : do_hangup();
}

int btc_ag_set_in_band_ring(bool on) {
    if (!a.started) {
        return BTC_ERR_STATE;
    }
    BTC_LOCK();
    a.in_band_ring = on;
    BTC_UNLOCK();
    if (a.connected) {
        return btc_err(esp_hf_ag_bsir(a.peer, on ? ESP_HF_IN_BAND_RINGTONE_PROVIDED : ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED));
    }
    return BTC_OK;
}

int btc_ag_voice_recognition(bool on) {
    int err = ready();
    if (err) {
        return err;
    }
    err = btc_err(esp_hf_ag_vra_control(a.peer, on ? ESP_HF_VR_STATE_ENABLED : ESP_HF_VR_STATE_DISABLED));
    if (err == BTC_OK) {
        BTC_LOCK();
        a.voice_recognition = on;
        BTC_UNLOCK();
        mark(BTC_CHANGED_PHONE);
    }
    return err;
}

static int set_gain(esp_hf_volume_control_target_t target, int *field, int gain) {
    int err = ready();
    if (err) {
        return err;
    }
    if (gain < 0 || gain > 15) {
        return BTC_ERR_ARG;
    }
    err = btc_err(esp_hf_ag_volume_control(a.peer, target, gain));
    if (err == BTC_OK) {
        set_int(field, gain, BTC_CHANGED_PHONE);
    }
    return err;
}

int btc_ag_set_speaker_gain(int gain) {
    return set_gain(ESP_HF_VOLUME_CONTROL_TARGET_SPK, &a.speaker_gain, gain);
}

int btc_ag_set_mic_gain(int gain) {
    return set_gain(ESP_HF_VOLUME_CONTROL_TARGET_MIC, &a.mic_gain, gain);
}

size_t btc_ag_dtmf(char *dst, size_t size) {
    BTC_LOCK();
    size_t n = a.dtmf_len < size ? a.dtmf_len : size;
    memcpy(dst, a.dtmf, n);
    memmove(a.dtmf, a.dtmf + n, a.dtmf_len - n);
    a.dtmf_len -= n;
    BTC_UNLOCK();
    return n;
}

// ---- audio ----

size_t btc_ag_read(int16_t *dst, size_t frames, uint8_t channels) {
    channels = channels == 2 ? 2 : 1;
    if (!a.audio) {
        memset(dst, 0, frames * channels * sizeof(int16_t));
        return 0;
    }
    return btc_ring_read(&a.ring, dst, frames, channels, 16129);
}

size_t btc_ag_write(const int16_t *pcm, size_t frames) {
    #if BTC_AG_AUDIO
    if (!a.started || !a.audio || a.out == NULL) {
        return 0;
    }
    uint32_t w = a.out_w;
    uint32_t used = w - __atomic_load_n(&a.out_r, __ATOMIC_ACQUIRE);
    uint32_t space = a.out_frames - used;
    size_t n = frames < space ? frames : space;
    for (size_t i = 0; i < n; i++) {
        a.out[(w + i) % a.out_frames] = pcm[i];
    }
    __atomic_store_n(&a.out_w, w + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
    #else
    (void)pcm;
    (void)frames;
    return 0;
    #endif
}

// ---- state ----

uint32_t btc_ag_changes(void) {
    BTC_LOCK();
    uint32_t c = a.changes;
    a.changes = 0;
    BTC_UNLOCK();
    return c;
}

void btc_ag_status(btc_ag_status_t *out) {
    BTC_LOCK();
    out->connected = a.connected;
    memcpy(out->peer, a.peer, sizeof(btc_addr_t));
    out->peer_features = a.peer_features;
    out->service = a.service;
    out->signal = a.signal;
    out->roaming = a.roaming;
    out->battery = a.battery;
    out->call = a.call;
    out->in_band_ring = a.in_band_ring;
    out->voice_recognition = a.voice_recognition;
    out->nrec = a.nrec;
    out->speaker_gain = a.speaker_gain;
    out->mic_gain = a.mic_gain;
    out->audio = a.audio;
    out->sample_rate = a.sample_rate;
    out->wideband = a.wideband;
    out->sent = a.sent;
    out->silent = a.silent;
    BTC_UNLOCK();
}

void btc_ag_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&a.ring, out);
}

size_t btc_ag_text(btc_ag_text_t which, char *dst, size_t size) {
    if (which >= BTC_AG_TEXT_COUNT) {
        return 0;
    }
    return btc_get_text(a.text[which], dst, size);
}

#else   // no audio gateway in this build

int btc_ag_start(uint32_t ring_ms) {
    (void)ring_ms;
    return BTC_ERR_UNSUPPORTED;
}
void btc_ag_stop(void) {
}
bool btc_ag_started(void) {
    return false;
}
int btc_ag_connect(const uint8_t *addr) {
    (void)addr;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_connect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_disconnect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_set_indicators(int service, int signal, int roaming, int battery) {
    (void)service; (void)signal; (void)roaming; (void)battery;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_set_text(btc_ag_text_t which, const char *text) {
    (void)which; (void)text;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_ring(const char *number) {
    (void)number;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_dial(const char *number) {
    (void)number;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_alerting(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_answer(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_hangup(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_set_in_band_ring(bool on) {
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_voice_recognition(bool on) {
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_set_speaker_gain(int gain) {
    (void)gain;
    return BTC_ERR_UNSUPPORTED;
}
int btc_ag_set_mic_gain(int gain) {
    (void)gain;
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_ag_dtmf(char *dst, size_t size) {
    (void)dst; (void)size;
    return 0;
}
size_t btc_ag_read(int16_t *dst, size_t frames, uint8_t channels) {
    memset(dst, 0, frames * (channels == 2 ? 2 : 1) * sizeof(int16_t));
    return 0;
}
size_t btc_ag_write(const int16_t *pcm, size_t frames) {
    (void)pcm; (void)frames;
    return 0;
}
uint32_t btc_ag_changes(void) {
    return 0;
}
void btc_ag_status(btc_ag_status_t *out) {
    memset(out, 0, sizeof(*out));
}
void btc_ag_stats(btc_ring_stats_t *out) {
    memset(out, 0, sizeof(*out));
}
size_t btc_ag_text(btc_ag_text_t which, char *dst, size_t size) {
    (void)which;
    if (size) {
        dst[0] = 0;
    }
    return 0;
}

#endif
