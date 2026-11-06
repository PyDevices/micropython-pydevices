// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio: A2DP sink and AVRCP on Bluedroid. See include/btaudio.h.
//
// Bluedroid calls every callback here from its own task. The audio callback
// is the only writer of the ring and btaudio_read() its only reader, so the
// ring needs no lock; everything else that callbacks and readers share sits
// under s_lock.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"

#include "btaudio.h"

static const char *TAG = "btaudio";

#define TEXT_MAX 128
#define DEFAULT_RING_FRAMES 24576   // about 0.56 s at 44.1 kHz
#define MIN_RING_FRAMES 2048
#define DEFAULT_VOLUME 100

typedef struct {
    // The ring: stereo frames, written by the A2DP data callback only and
    // read by btaudio_read() only. Positions count frames and wrap modulo
    // 2^32; the index into `ring` is position % capacity.
    int16_t *ring;
    uint32_t capacity;
    volatile uint32_t write_pos;
    volatile uint32_t read_pos;
    // Set by the callbacks, acted on by the reader: start over from empty.
    volatile bool reset;
    // The reader's own state: false while it refills to half before playing.
    bool running;

    // Shared between callbacks and callers, under s_lock.
    bool connected;
    bool streaming;
    bool playing;
    uint8_t channels;
    uint32_t sample_rate;
    uint8_t volume;
    bool volume_notify;        // the sender asked to hear of volume changes
    uint32_t changes;
    uint16_t peak_l, peak_r;
    uint32_t underruns, overruns, dropped, repeated, received;
    uint16_t peer_events;      // AVRCP notifications the sender supports
    uint8_t tl;                // AVRCP transaction label
    esp_bd_addr_t peer;
    char text[BTAUDIO_TEXT_COUNT][TEXT_MAX];
} btaudio_state_t;

static btaudio_state_t s;
static bool s_started;
static bool s_reconnect;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void mark(uint32_t bits) {
    portENTER_CRITICAL(&s_lock);
    s.changes |= bits;
    portEXIT_CRITICAL(&s_lock);
}

static uint8_t next_tl(void) {
    portENTER_CRITICAL(&s_lock);
    uint8_t tl = s.tl;
    s.tl = (s.tl + 1) & ESP_AVRC_TRANS_LABEL_MAX;
    portEXIT_CRITICAL(&s_lock);
    return tl;
}

static void set_text(btaudio_text_t which, const uint8_t *src, size_t len) {
    if (len >= TEXT_MAX) {
        len = TEXT_MAX - 1;
    }
    portENTER_CRITICAL(&s_lock);
    memcpy(s.text[which], src, len);
    s.text[which][len] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void clear_track(void) {
    portENTER_CRITICAL(&s_lock);
    s.text[BTAUDIO_TEXT_TITLE][0] = 0;
    s.text[BTAUDIO_TEXT_ARTIST][0] = 0;
    s.text[BTAUDIO_TEXT_ALBUM][0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

// ---- AVRCP controller: what the sender is playing, and its transport ----

static bool peer_supports(uint8_t event_id) {
    esp_avrc_rn_evt_cap_mask_t caps = {.bits = s.peer_events};
    return esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps, event_id);
}

static void request_metadata(void) {
    esp_avrc_ct_send_metadata_cmd(next_tl(),
        ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM);
}

// AVRCP notifications fire once: each is registered again after it fires.
static void register_notification(uint8_t event_id) {
    if (peer_supports(event_id)) {
        esp_avrc_ct_send_register_notification_cmd(next_tl(), event_id, 0);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
            } else {
                portENTER_CRITICAL(&s_lock);
                s.peer_events = 0;
                s.playing = false;
                portEXIT_CRITICAL(&s_lock);
                clear_track();
                mark(BTAUDIO_CHANGED_TRACK | BTAUDIO_CHANGED_PLAYING);
            }
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
            portENTER_CRITICAL(&s_lock);
            s.peer_events = param->get_rn_caps_rsp.evt_set.bits;
            portEXIT_CRITICAL(&s_lock);
            register_notification(ESP_AVRC_RN_TRACK_CHANGE);
            register_notification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
            request_metadata();
            break;
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            // attr_text is only valid during this call.
            btaudio_text_t which;
            switch (param->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    which = BTAUDIO_TEXT_TITLE;
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    which = BTAUDIO_TEXT_ARTIST;
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    which = BTAUDIO_TEXT_ALBUM;
                    break;
                default:
                    return;
            }
            set_text(which, param->meta_rsp.attr_text, param->meta_rsp.attr_length);
            mark(BTAUDIO_CHANGED_TRACK);
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            switch (param->change_ntf.event_id) {
                case ESP_AVRC_RN_TRACK_CHANGE:
                    request_metadata();
                    register_notification(ESP_AVRC_RN_TRACK_CHANGE);
                    break;
                case ESP_AVRC_RN_PLAY_STATUS_CHANGE: {
                    bool playing = param->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING;
                    portENTER_CRITICAL(&s_lock);
                    bool changed = s.playing != playing;
                    s.playing = playing;
                    portEXIT_CRITICAL(&s_lock);
                    if (changed) {
                        mark(BTAUDIO_CHANGED_PLAYING);
                    }
                    register_notification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
                    break;
                }
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

// ---- AVRCP target: the sender's volume slider ----

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            portENTER_CRITICAL(&s_lock);
            s.volume = param->set_abs_vol.volume & 0x7f;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_VOLUME);
            break;
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                esp_avrc_rn_param_t rn = {0};
                portENTER_CRITICAL(&s_lock);
                s.volume_notify = true;
                rn.volume = s.volume;
                portEXIT_CRITICAL(&s_lock);
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
            }
            break;
        default:
            break;
    }
}

// ---- the last sender, kept in NVS so a restart can call it back ----

#define NVS_NAMESPACE "btaudio"
#define NVS_KEY_LAST "last"

static void save_last_sender(const uint8_t *bda) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_LAST, bda, sizeof(esp_bd_addr_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

// Call the last sender back, as a speaker does when it's switched on. Only a
// sender we're still bonded with: one that unpaired would just refuse.
static void reconnect_last_sender(void) {
    esp_bd_addr_t last;
    size_t len = sizeof(last);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LAST, last, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(last)) {
        return;
    }
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_bd_addr_t *bonded = malloc(n * sizeof(esp_bd_addr_t));
    if (bonded == NULL) {
        return;
    }
    if (esp_bt_gap_get_bond_device_list(&n, bonded) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (memcmp(bonded[i], last, sizeof(last)) == 0) {
                ESP_LOGI(TAG, "calling back the last sender");
                esp_a2d_sink_connect(last);
                break;
            }
        }
    }
    free(bonded);
}

// ---- A2DP: the link, the stream's format, and the audio ----

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_PROF_STATE_EVT:
            if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS && s_reconnect) {
                reconnect_last_sender();
            }
            break;
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                const uint8_t *a = param->conn_stat.remote_bda;
                char peer[18];
                snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
                set_text(BTAUDIO_TEXT_PEER, (const uint8_t *)peer, strlen(peer));
                portENTER_CRITICAL(&s_lock);
                memcpy(s.peer, a, sizeof(esp_bd_addr_t));
                s.connected = true;
                portEXIT_CRITICAL(&s_lock);
                save_last_sender(a);
                // One sender at a time: hide until it leaves.
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                mark(BTAUDIO_CHANGED_CONNECTION);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                portENTER_CRITICAL(&s_lock);
                s.connected = false;
                s.streaming = false;
                s.volume_notify = false;
                portEXIT_CRITICAL(&s_lock);
                set_text(BTAUDIO_TEXT_PEER, (const uint8_t *)"", 0);
                s.reset = true;
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                mark(BTAUDIO_CHANGED_CONNECTION | BTAUDIO_CHANGED_STREAM);
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            bool streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            portENTER_CRITICAL(&s_lock);
            s.streaming = streaming;
            portEXIT_CRITICAL(&s_lock);
            s.reset = true;
            mark(BTAUDIO_CHANGED_STREAM);
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            const esp_a2d_mcc_t *mcc = &param->audio_cfg.mcc;
            if (mcc->type != ESP_A2D_MCT_SBC) {
                break;
            }
            uint8_t sf = mcc->cie.sbc_info.samp_freq;
            uint32_t rate = 16000;
            if (sf & ESP_A2D_SBC_CIE_SF_32K) {
                rate = 32000;
            } else if (sf & ESP_A2D_SBC_CIE_SF_44K) {
                rate = 44100;
            } else if (sf & ESP_A2D_SBC_CIE_SF_48K) {
                rate = 48000;
            }
            uint8_t channels = (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
            portENTER_CRITICAL(&s_lock);
            s.sample_rate = rate;
            s.channels = channels;
            portEXIT_CRITICAL(&s_lock);
            s.reset = true;
            mark(BTAUDIO_CHANGED_FORMAT);
            ESP_LOGI(TAG, "stream: SBC, %u Hz, %u channel(s)", (unsigned)rate, channels);
            break;
        }
        default:
            break;
    }
}

static inline uint16_t mag(int16_t v) {
    return v < 0 ? (uint16_t)(v == INT16_MIN ? 32767 : -v) : (uint16_t)v;
}

// Decoded PCM, 16-bit, interleaved when stereo. Mono is widened to stereo
// here, so the ring and every reader see one shape.
static void a2d_data_cb(const uint8_t *buf, uint32_t len) {
    const int16_t *in = (const int16_t *)buf;
    uint8_t channels = s.channels ? s.channels : 2;
    uint32_t frames = len / (2u * channels);
    uint32_t wp = s.write_pos;
    uint32_t space = s.capacity - (wp - s.read_pos);
    uint32_t n = frames < space ? frames : space;
    uint16_t pl = 0, pr = 0;
    for (uint32_t i = 0; i < n; i++) {
        int16_t l = in[i * channels];
        int16_t r = channels == 2 ? in[i * 2 + 1] : l;
        int16_t *dst = &s.ring[((wp + i) % s.capacity) * 2];
        dst[0] = l;
        dst[1] = r;
        uint16_t ml = mag(l), mr = mag(r);
        if (ml > pl) {
            pl = ml;
        }
        if (mr > pr) {
            pr = mr;
        }
    }
    __atomic_store_n(&s.write_pos, wp + n, __ATOMIC_RELEASE);
    portENTER_CRITICAL(&s_lock);
    if (pl > s.peak_l) {
        s.peak_l = pl;
    }
    if (pr > s.peak_r) {
        s.peak_r = pr;
    }
    s.received += n;
    if (n < frames) {
        s.overruns++;
    }
    portEXIT_CRITICAL(&s_lock);
}

// ---- GAP: pairing with no PIN to type ----

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
            } else {
                ESP_LOGW(TAG, "pairing failed: %d", param->auth_cmpl.stat);
            }
            break;
#if CONFIG_BT_SSP_ENABLED
        case ESP_BT_GAP_CFM_REQ_EVT:
            // A speaker has no screen to compare numbers on: accept.
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
#endif
        default:
            break;
    }
}

// ---- start and stop ----

esp_err_t btaudio_start(const char *name, size_t ring_frames, bool reconnect) {
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ring_frames == 0) {
        ring_frames = DEFAULT_RING_FRAMES;
    }
    if (ring_frames < MIN_RING_FRAMES) {
        ring_frames = MIN_RING_FRAMES;
    }
    memset(&s, 0, sizeof(s));
    s.capacity = ring_frames;
    s.volume = DEFAULT_VOLUME;
    s.channels = 2;
    s_reconnect = reconnect;
    size_t bytes = ring_frames * 2 * sizeof(int16_t);
    s.ring = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.ring == NULL) {
        s.ring = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (s.ring == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        // Classic only: give the BLE half of the controller's memory back.
        // Harmless if it has gone already.
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
    }
    if (err == ESP_OK && esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    }
    if (err == ESP_OK) {
        esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        err = esp_bluedroid_init_with_cfg(&bcfg);
    }
    if (err == ESP_OK) {
        err = esp_bluedroid_enable();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth didn't start: %s", esp_err_to_name(err));
        heap_caps_free(s.ring);
        s.ring = NULL;
        return err;
    }

    esp_bt_gap_set_device_name(name);
    esp_bt_gap_register_callback(gap_cb);
#if CONFIG_BT_SSP_ENABLED
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
#endif
    // Legacy pairing, for senders without SSP: the PIN every speaker uses.
    esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin);

    // AVRCP first, then A2DP, as Bluedroid asks.
    esp_avrc_ct_register_callback(avrc_ct_cb);
    esp_avrc_ct_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_tg_init();
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    err = esp_a2d_sink_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink didn't start: %s", esp_err_to_name(err));
    }

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    s_started = true;
    return err;
}

void btaudio_stop(void) {
    if (!s_started) {
        return;
    }
    s_started = false;
    esp_a2d_sink_deinit();
    esp_avrc_tg_deinit();
    esp_avrc_ct_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    // Bluedroid's task may still be finishing a callback that writes the ring.
    vTaskDelay(pdMS_TO_TICKS(50));
    heap_caps_free(s.ring);
    s.ring = NULL;
    s.capacity = 0;
}

bool btaudio_started(void) {
    return s_started;
}

// ---- reading ----

size_t btaudio_read(int16_t *dst, size_t frames) {
    if (!s_started || s.ring == NULL) {
        memset(dst, 0, frames * 4);
        return 0;
    }
    if (s.reset) {
        s.reset = false;
        __atomic_store_n(&s.read_pos, __atomic_load_n(&s.write_pos, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        s.running = false;
    }
    uint32_t rp = s.read_pos;
    uint32_t avail = __atomic_load_n(&s.write_pos, __ATOMIC_ACQUIRE) - rp;
    uint32_t target = s.capacity / 2;
    uint32_t slack = s.capacity / 8;

    if (!s.running) {
        if (avail < target) {
            memset(dst, 0, frames * 4);
            return 0;
        }
        s.running = true;
    }

    // Take one frame more or less than asked for when the ring has drifted
    // away from half full: the sender's clock and ours never quite agree.
    size_t take = frames;
    int adjust = 0;
    if (avail > target + slack && avail > frames + 1) {
        take = frames + 1;
        adjust = 1;
    } else if (avail < target - slack && frames > 1) {
        take = frames - 1;
        adjust = -1;
    }
    size_t from_ring = take <= avail ? take : avail;

    portENTER_CRITICAL(&s_lock);
    int32_t gain = (int32_t)s.volume * s.volume;    // 0..16129, about a dB taper
    portEXIT_CRITICAL(&s_lock);

    size_t out = 0;
    for (size_t i = 0; i < from_ring && out < frames; i++) {
        if (adjust > 0 && i == from_ring / 2) {
            continue;   // drop this frame
        }
        const int16_t *src = &s.ring[((rp + i) % s.capacity) * 2];
        dst[out * 2] = (int16_t)(((int32_t)src[0] * gain) / 16129);
        dst[out * 2 + 1] = (int16_t)(((int32_t)src[1] * gain) / 16129);
        out++;
    }
    size_t played = out;
    if (adjust < 0 && out > 0 && out < frames) {
        dst[out * 2] = dst[(out - 1) * 2];  // repeat the last frame
        dst[out * 2 + 1] = dst[(out - 1) * 2 + 1];
        out++;
    }
    __atomic_store_n(&s.read_pos, rp + from_ring, __ATOMIC_RELEASE);

    portENTER_CRITICAL(&s_lock);
    if (out < frames) {
        s.underruns++;
    } else if (adjust > 0) {
        s.dropped++;
    } else if (adjust < 0) {
        s.repeated++;
    }
    portEXIT_CRITICAL(&s_lock);
    if (out < frames) {
        memset(&dst[out * 2], 0, (frames - out) * 4);
        s.running = false;  // ran dry: refill to half before playing on
    }
    return played;
}

// ---- state for callers ----

uint32_t btaudio_changes(void) {
    portENTER_CRITICAL(&s_lock);
    uint32_t c = s.changes;
    s.changes = 0;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

void btaudio_status(btaudio_status_t *out) {
    uint32_t buffered = __atomic_load_n(&s.write_pos, __ATOMIC_ACQUIRE) - s.read_pos;
    portENTER_CRITICAL(&s_lock);
    out->connected = s.connected;
    out->streaming = s.streaming;
    out->playing = s.playing;
    out->sample_rate = s.sample_rate;
    out->volume = s.volume;
    out->buffered = buffered;
    out->capacity = s.capacity;
    out->underruns = s.underruns;
    out->overruns = s.overruns;
    out->dropped = s.dropped;
    out->repeated = s.repeated;
    out->received = s.received;
    portEXIT_CRITICAL(&s_lock);
}

size_t btaudio_text(btaudio_text_t which, char *dst, size_t size) {
    if (size == 0 || which >= BTAUDIO_TEXT_COUNT) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    strncpy(dst, s.text[which], size - 1);
    portEXIT_CRITICAL(&s_lock);
    dst[size - 1] = 0;
    return strlen(dst);
}

void btaudio_levels(uint16_t *left, uint16_t *right) {
    portENTER_CRITICAL(&s_lock);
    *left = s.peak_l;
    *right = s.peak_r;
    s.peak_l = 0;
    s.peak_r = 0;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t btaudio_command(btaudio_cmd_t cmd) {
    static const uint8_t keys[] = {
        [BTAUDIO_CMD_PLAY] = ESP_AVRC_PT_CMD_PLAY,
        [BTAUDIO_CMD_PAUSE] = ESP_AVRC_PT_CMD_PAUSE,
        [BTAUDIO_CMD_NEXT] = ESP_AVRC_PT_CMD_FORWARD,
        [BTAUDIO_CMD_PREVIOUS] = ESP_AVRC_PT_CMD_BACKWARD,
        [BTAUDIO_CMD_STOP] = ESP_AVRC_PT_CMD_STOP,
    };
    if (!s_started || (unsigned)cmd >= sizeof(keys)) {
        return ESP_ERR_INVALID_STATE;
    }
    // A button is a press and a release.
    esp_err_t err = esp_avrc_ct_send_passthrough_cmd(next_tl(), keys[cmd], ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (err == ESP_OK) {
        err = esp_avrc_ct_send_passthrough_cmd(next_tl(), keys[cmd], ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
    return err;
}

void btaudio_set_volume(uint8_t volume) {
    volume &= 0x7f;
    portENTER_CRITICAL(&s_lock);
    s.volume = volume;
    bool notify = s.volume_notify;
    s.volume_notify = false;
    portEXIT_CRITICAL(&s_lock);
    if (s_started && notify) {
        esp_avrc_rn_param_t rn = {0};
        rn.volume = volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
    }
    mark(BTAUDIO_CHANGED_VOLUME);
}

esp_err_t btaudio_disconnect(void) {
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_bd_addr_t peer;
    portENTER_CRITICAL(&s_lock);
    bool connected = s.connected;
    memcpy(peer, s.peer, sizeof(peer));
    portEXIT_CRITICAL(&s_lock);
    return connected ? esp_a2d_sink_disconnect(peer) : ESP_OK;
}
