// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio: A2DP sink, AVRCP (cover art included) and HFP hands-free on
// Bluedroid. See include/btaudio.h.
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
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#if CONFIG_BT_HFP_CLIENT_ENABLE
#include "esp_hf_client_api.h"
#define BTAUDIO_HFP 1
#else
#define BTAUDIO_HFP 0
#endif

#if CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
#define BTAUDIO_COVER 1
#else
#define BTAUDIO_COVER 0
#endif

#include "btaudio.h"

static const char *TAG = "btaudio";

#define TEXT_MAX 128
#define DEFAULT_RING_FRAMES 24576   // about 0.56 s at 44.1 kHz
#define MIN_RING_FRAMES 2048
#define DEFAULT_VOLUME 100
#define POLL_US 1000000             // play status and signal, once a second
#define COVER_MAX (256 * 1024)      // a thumbnail is a few tens of KB
#define HANDLE_LEN 7                // AVRCP image handles are 7 digits
#define AVRCP_UNKNOWN_MS 0xffffffffu

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

    // AVRCP
    bool avrc_connected;
    int play_status;
    int track_number, track_count;
    int64_t duration_ms;
    int64_t pos_ms;            // last reported position
    int64_t pos_at_us;         // when it was reported
    int battery;
    uint32_t avrcp_features;
    uint16_t avrcp_tg_flags;

    // Cover art: the image handle the metadata named, the one we fetched,
    // and the JPEG assembled from the data events.
    bool cover_connected;
    bool cover_getting;
    uint8_t cover_handle[HANDLE_LEN];
    bool cover_has_handle;
    uint8_t cover_fetched[HANDLE_LEN];
    uint8_t *cover;            // the finished image, under s_lock
    size_t cover_len;
    uint8_t *cover_build;      // being assembled, Bluedroid's task only
    size_t cover_build_len;

    // Link
    int rssi_delta;
    uint8_t sbc_channel_mode, sbc_block_length, sbc_subbands, sbc_allocation;
    uint8_t sbc_min_bitpool, sbc_max_bitpool;
    uint16_t delay_ms;

    // HFP
    bool hf_connected;
    int service, signal, roaming, phone_battery;
    int ind_call, ind_setup, held;
    bool waiting, ringing, voice, in_band_ring;
    int speaker_gain, mic_gain;
} btaudio_state_t;

static btaudio_state_t s;
static bool s_started;
static bool s_reconnect;
static esp_timer_handle_t s_poll;

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

static void set_cstr(btaudio_text_t which, const char *src) {
    set_text(which, (const uint8_t *)(src ? src : ""), src ? strlen(src) : 0);
}

// A decimal attribute (track number, count, playing time), or UNKNOWN.
static int64_t parse_number(const uint8_t *src, int len) {
    int64_t v = 0;
    if (len <= 0) {
        return BTAUDIO_UNKNOWN;
    }
    for (int i = 0; i < len; i++) {
        if (src[i] < '0' || src[i] > '9') {
            return BTAUDIO_UNKNOWN;
        }
        v = v * 10 + (src[i] - '0');
    }
    return v;
}

static void set_position(uint32_t ms) {
    portENTER_CRITICAL(&s_lock);
    s.pos_ms = ms == AVRCP_UNKNOWN_MS ? BTAUDIO_UNKNOWN : (int64_t)ms;
    s.pos_at_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
}

// ---- cover art ----

static void cover_set(uint8_t *jpeg, size_t len) {
    portENTER_CRITICAL(&s_lock);
    uint8_t *old = s.cover;
    s.cover = jpeg;
    s.cover_len = jpeg ? len : 0;
    portEXIT_CRITICAL(&s_lock);
    free(old);
    mark(BTAUDIO_CHANGED_COVER);
}

static void cover_clear(void) {
    portENTER_CRITICAL(&s_lock);
    bool had = s.cover != NULL;
    portEXIT_CRITICAL(&s_lock);
    if (had) {
        cover_set(NULL, 0);
    }
}

#if BTAUDIO_COVER
// Fetch the thumbnail for the handle the metadata named, one at a time and
// once per image: senders give each image its own handle.
static void cover_fetch(void) {
    if (!s.cover_connected || s.cover_getting || !s.cover_has_handle) {
        return;
    }
    if (memcmp(s.cover_handle, s.cover_fetched, HANDLE_LEN) == 0) {
        return;
    }
    memcpy(s.cover_fetched, s.cover_handle, HANDLE_LEN);
    free(s.cover_build);
    s.cover_build = NULL;
    s.cover_build_len = 0;
    if (esp_avrc_ct_cover_art_get_linked_thumbnail(s.cover_fetched) == ESP_OK) {
        s.cover_getting = true;
    }
}

static void cover_data(const esp_avrc_ct_cb_param_t *param) {
    s.cover_getting = !param->cover_art_data.final;
    if (param->cover_art_data.status != ESP_BT_STATUS_SUCCESS) {
        free(s.cover_build);
        s.cover_build = NULL;
        s.cover_build_len = 0;
        s.cover_getting = false;
        cover_fetch();
        return;
    }
    size_t add = param->cover_art_data.data_len;
    if (add && s.cover_build_len + add <= COVER_MAX) {
        uint8_t *grown = heap_caps_realloc(s.cover_build, s.cover_build_len + add, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (grown == NULL) {
            grown = realloc(s.cover_build, s.cover_build_len + add);
        }
        if (grown != NULL) {
            memcpy(grown + s.cover_build_len, param->cover_art_data.p_data, add);
            s.cover_build = grown;
            s.cover_build_len += add;
        }
    }
    if (param->cover_art_data.final) {
        uint8_t *jpeg = s.cover_build;
        size_t len = s.cover_build_len;
        s.cover_build = NULL;
        s.cover_build_len = 0;
        if (jpeg && len) {
            cover_set(jpeg, len);
        } else {
            free(jpeg);
        }
        cover_fetch();     // a newer track may have named another image
    }
}
#endif

// ---- AVRCP controller: what the sender is playing, and its transport ----

static bool peer_supports(uint8_t event_id) {
    esp_avrc_rn_evt_cap_mask_t caps = {.bits = s.peer_events};
    return esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps, event_id);
}

static void request_metadata(void) {
    uint8_t mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM
        | ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE
        | ESP_AVRC_MD_ATTR_PLAYING_TIME;
    #if BTAUDIO_COVER
    mask |= ESP_AVRC_MD_ATTR_COVER_ART;
    #endif
    esp_avrc_ct_send_metadata_cmd(next_tl(), mask);
}

// AVRCP notifications fire once: each is registered again after it fires.
static void register_notification(uint8_t event_id) {
    if (peer_supports(event_id)) {
        // The position notification takes its interval, in seconds.
        uint32_t param = event_id == ESP_AVRC_RN_PLAY_POS_CHANGED ? 1 : 0;
        esp_avrc_ct_send_register_notification_cmd(next_tl(), event_id, param);
    }
}

static void clear_track(void) {
    portENTER_CRITICAL(&s_lock);
    s.text[BTAUDIO_TEXT_TITLE][0] = 0;
    s.text[BTAUDIO_TEXT_ARTIST][0] = 0;
    s.text[BTAUDIO_TEXT_ALBUM][0] = 0;
    s.text[BTAUDIO_TEXT_GENRE][0] = 0;
    s.track_number = BTAUDIO_UNKNOWN;
    s.track_count = BTAUDIO_UNKNOWN;
    s.duration_ms = BTAUDIO_UNKNOWN;
    s.pos_ms = BTAUDIO_UNKNOWN;
    s.cover_has_handle = false;
    portEXIT_CRITICAL(&s_lock);
}

static void set_play_status(int status) {
    portENTER_CRITICAL(&s_lock);
    bool changed = s.play_status != status;
    // Freeze the position where it is when playback stops moving it.
    if (changed && s.play_status == BTAUDIO_PLAY_PLAYING && s.pos_ms >= 0) {
        int64_t now = esp_timer_get_time();
        s.pos_ms += (now - s.pos_at_us) / 1000;
        s.pos_at_us = now;
    } else if (changed && status == BTAUDIO_PLAY_PLAYING) {
        s.pos_at_us = esp_timer_get_time();
    }
    s.play_status = status;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        mark(BTAUDIO_CHANGED_PLAYING);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                portENTER_CRITICAL(&s_lock);
                s.avrc_connected = true;
                portEXIT_CRITICAL(&s_lock);
                esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
            } else {
                portENTER_CRITICAL(&s_lock);
                s.avrc_connected = false;
                s.peer_events = 0;
                s.play_status = BTAUDIO_UNKNOWN;
                s.battery = BTAUDIO_UNKNOWN;
                s.avrcp_features = 0;
                s.avrcp_tg_flags = 0;
                s.cover_connected = false;
                s.cover_getting = false;
                memset(s.cover_fetched, 0, HANDLE_LEN);
                portEXIT_CRITICAL(&s_lock);
                clear_track();
                cover_clear();
                mark(BTAUDIO_CHANGED_TRACK | BTAUDIO_CHANGED_PLAYING | BTAUDIO_CHANGED_PEER);
            }
            break;
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
            portENTER_CRITICAL(&s_lock);
            s.avrcp_features = param->rmt_feats.feat_mask;
            s.avrcp_tg_flags = param->rmt_feats.tg_feat_flag;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_PEER);
            #if BTAUDIO_COVER
            if ((param->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) && !s.cover_connected) {
                esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
            }
            #endif
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
            portENTER_CRITICAL(&s_lock);
            s.peer_events = param->get_rn_caps_rsp.evt_set.bits;
            portEXIT_CRITICAL(&s_lock);
            register_notification(ESP_AVRC_RN_TRACK_CHANGE);
            register_notification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
            register_notification(ESP_AVRC_RN_PLAY_POS_CHANGED);
            register_notification(ESP_AVRC_RN_BATTERY_STATUS_CHANGE);
            register_notification(ESP_AVRC_RN_TRACK_REACHED_END);
            register_notification(ESP_AVRC_RN_TRACK_REACHED_START);
            request_metadata();
            esp_avrc_ct_send_get_play_status_cmd(next_tl());
            break;
        case ESP_AVRC_CT_METADATA_RSP_EVT: {
            // attr_text is only valid during this call.
            const uint8_t *text = param->meta_rsp.attr_text;
            int len = param->meta_rsp.attr_length;
            switch (param->meta_rsp.attr_id) {
                case ESP_AVRC_MD_ATTR_TITLE:
                    set_text(BTAUDIO_TEXT_TITLE, text, len);
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    set_text(BTAUDIO_TEXT_ARTIST, text, len);
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    set_text(BTAUDIO_TEXT_ALBUM, text, len);
                    break;
                case ESP_AVRC_MD_ATTR_GENRE:
                    set_text(BTAUDIO_TEXT_GENRE, text, len);
                    break;
                case ESP_AVRC_MD_ATTR_TRACK_NUM: {
                    int64_t v = parse_number(text, len);
                    portENTER_CRITICAL(&s_lock);
                    s.track_number = v > 0 ? (int)v : BTAUDIO_UNKNOWN;
                    portEXIT_CRITICAL(&s_lock);
                    break;
                }
                case ESP_AVRC_MD_ATTR_NUM_TRACKS: {
                    int64_t v = parse_number(text, len);
                    portENTER_CRITICAL(&s_lock);
                    s.track_count = v > 0 ? (int)v : BTAUDIO_UNKNOWN;
                    portEXIT_CRITICAL(&s_lock);
                    break;
                }
                case ESP_AVRC_MD_ATTR_PLAYING_TIME: {
                    int64_t v = parse_number(text, len);
                    portENTER_CRITICAL(&s_lock);
                    s.duration_ms = v > 0 ? v : BTAUDIO_UNKNOWN;
                    portEXIT_CRITICAL(&s_lock);
                    break;
                }
                #if BTAUDIO_COVER
                case ESP_AVRC_MD_ATTR_COVER_ART:
                    if (len == HANDLE_LEN) {
                        memcpy(s.cover_handle, text, HANDLE_LEN);
                        s.cover_has_handle = true;
                        cover_fetch();
                    } else {
                        // The track has no image.
                        s.cover_has_handle = false;
                        memset(s.cover_fetched, 0, HANDLE_LEN);
                        cover_clear();
                    }
                    return;
                #endif
                default:
                    return;
            }
            mark(BTAUDIO_CHANGED_TRACK);
            break;
        }
        case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT: {
            uint32_t length = param->play_status_rsp.song_length;
            portENTER_CRITICAL(&s_lock);
            if (length != AVRCP_UNKNOWN_MS && length != 0) {
                s.duration_ms = length;
            }
            portEXIT_CRITICAL(&s_lock);
            set_position(param->play_status_rsp.song_position);
            set_play_status(param->play_status_rsp.play_status);
            mark(BTAUDIO_CHANGED_POSITION);
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
            uint8_t id = param->change_ntf.event_id;
            switch (id) {
                case ESP_AVRC_RN_TRACK_CHANGE:
                    clear_track();
                    mark(BTAUDIO_CHANGED_TRACK);
                    request_metadata();
                    esp_avrc_ct_send_get_play_status_cmd(next_tl());
                    break;
                case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
                    set_play_status(param->change_ntf.event_parameter.playback);
                    esp_avrc_ct_send_get_play_status_cmd(next_tl());
                    break;
                case ESP_AVRC_RN_PLAY_POS_CHANGED:
                    set_position(param->change_ntf.event_parameter.play_pos);
                    mark(BTAUDIO_CHANGED_POSITION);
                    break;
                case ESP_AVRC_RN_BATTERY_STATUS_CHANGE:
                    portENTER_CRITICAL(&s_lock);
                    s.battery = param->change_ntf.event_parameter.batt;
                    portEXIT_CRITICAL(&s_lock);
                    mark(BTAUDIO_CHANGED_PEER);
                    break;
                case ESP_AVRC_RN_TRACK_REACHED_END:
                case ESP_AVRC_RN_TRACK_REACHED_START:
                    esp_avrc_ct_send_get_play_status_cmd(next_tl());
                    break;
                default:
                    return;
            }
            register_notification(id);
            break;
        }
        #if BTAUDIO_COVER
        case ESP_AVRC_CT_COVER_ART_STATE_EVT:
            s.cover_connected = param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
            s.cover_getting = false;
            mark(BTAUDIO_CHANGED_PEER);
            if (s.cover_connected) {
                // Ask again: the handle may have come before the channel.
                memset(s.cover_fetched, 0, HANDLE_LEN);
                esp_avrc_ct_send_metadata_cmd(next_tl(), ESP_AVRC_MD_ATTR_COVER_ART);
            }
            break;
        case ESP_AVRC_CT_COVER_ART_DATA_EVT:
            cover_data(param);
            break;
        #endif
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

// ---- HFP hands-free: the phone's indicators and calls ----

#if BTAUDIO_HFP
static void reset_phone(void) {
    portENTER_CRITICAL(&s_lock);
    s.hf_connected = false;
    s.service = s.signal = s.roaming = s.phone_battery = BTAUDIO_UNKNOWN;
    s.ind_call = s.ind_setup = s.held = 0;
    s.waiting = s.ringing = s.voice = s.in_band_ring = false;
    s.speaker_gain = s.mic_gain = BTAUDIO_UNKNOWN;
    s.text[BTAUDIO_TEXT_OPERATOR][0] = 0;
    s.text[BTAUDIO_TEXT_SUBSCRIBER][0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void set_phone(int *field, int value) {
    portENTER_CRITICAL(&s_lock);
    bool changed = *field != value;
    *field = value;
    portEXIT_CRITICAL(&s_lock);
    if (changed) {
        mark(BTAUDIO_CHANGED_PHONE);
    }
}

// A call indicator moved: ask for the call list, which carries the number.
static void call_changed(void) {
    portENTER_CRITICAL(&s_lock);
    if (s.ind_call == 0 && s.ind_setup == 0) {
        s.ringing = false;
        s.waiting = false;
        s.held = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    mark(BTAUDIO_CHANGED_CALL);
    esp_hf_client_query_current_calls();
}

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param) {
    switch (event) {
        case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
            if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
                portENTER_CRITICAL(&s_lock);
                s.hf_connected = true;
                portEXIT_CRITICAL(&s_lock);
                esp_hf_client_query_current_operator_name();
                esp_hf_client_retrieve_subscriber_info();
                esp_hf_client_query_current_calls();
                mark(BTAUDIO_CHANGED_PHONE);
            } else if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
                reset_phone();
                mark(BTAUDIO_CHANGED_PHONE | BTAUDIO_CHANGED_CALL);
            }
            break;
        case ESP_HF_CLIENT_AUDIO_STATE_EVT:
            // No call audio here: hand the call straight back to the phone.
            if (param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED
                || param->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
                esp_hf_client_disconnect_audio(param->audio_stat.remote_bda);
            }
            break;
        case ESP_HF_CLIENT_BVRA_EVT:
            portENTER_CRITICAL(&s_lock);
            s.voice = param->bvra.value == ESP_HF_VR_STATE_ENABLED;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_CIND_CALL_EVT:
            s.ind_call = param->call.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
            s.ind_setup = param->call_setup.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
            s.held = param->call_held.status;
            call_changed();
            break;
        case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
            set_phone(&s.service, param->service_availability.status);
            break;
        case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
            set_phone(&s.signal, param->signal_strength.value);
            break;
        case ESP_HF_CLIENT_CIND_ROAMING_STATUS_EVT:
            set_phone(&s.roaming, param->roaming.status);
            break;
        case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
            set_phone(&s.phone_battery, param->battery_level.value);
            break;
        case ESP_HF_CLIENT_COPS_CURRENT_OPERATOR_EVT:
            set_cstr(BTAUDIO_TEXT_OPERATOR, param->cops.name);
            mark(BTAUDIO_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_CLIP_EVT:
            set_cstr(BTAUDIO_TEXT_CALLER, param->clip.number);
            mark(BTAUDIO_CHANGED_CALL);
            break;
        case ESP_HF_CLIENT_CCWA_EVT:
            portENTER_CRITICAL(&s_lock);
            s.waiting = true;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_CALL);
            break;
        case ESP_HF_CLIENT_CLCC_EVT:
            if (param->clcc.number && param->clcc.number[0]) {
                set_cstr(BTAUDIO_TEXT_CALLER, param->clcc.number);
                mark(BTAUDIO_CHANGED_CALL);
            }
            break;
        case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
            if (param->volume_control.type == ESP_HF_VOLUME_CONTROL_TARGET_SPK) {
                set_phone(&s.speaker_gain, param->volume_control.volume);
            } else {
                set_phone(&s.mic_gain, param->volume_control.volume);
            }
            break;
        case ESP_HF_CLIENT_CNUM_EVT:
            set_cstr(BTAUDIO_TEXT_SUBSCRIBER, param->cnum.number);
            mark(BTAUDIO_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_BSIR_EVT:
            portENTER_CRITICAL(&s_lock);
            s.in_band_ring = param->bsir.state == ESP_HF_CLIENT_IN_BAND_RINGTONE_PROVIDED;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_PHONE);
            break;
        case ESP_HF_CLIENT_RING_IND_EVT:
            portENTER_CRITICAL(&s_lock);
            s.ringing = true;
            portEXIT_CRITICAL(&s_lock);
            mark(BTAUDIO_CHANGED_CALL);
            break;
        default:
            break;
    }
}
#endif

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
                uint8_t *a = param->conn_stat.remote_bda;
                char peer[18];
                snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
                set_text(BTAUDIO_TEXT_PEER, (const uint8_t *)peer, strlen(peer));
                portENTER_CRITICAL(&s_lock);
                memcpy(s.peer, a, sizeof(esp_bd_addr_t));
                s.connected = true;
                portEXIT_CRITICAL(&s_lock);
                save_last_sender(a);
                esp_bt_gap_read_remote_name(a);
                #if BTAUDIO_HFP
                // A phone also takes calls: follow it as a hands-free unit.
                if (!s.hf_connected) {
                    esp_hf_client_connect(a);
                }
                #endif
                // One sender at a time: hide until it leaves.
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                mark(BTAUDIO_CHANGED_CONNECTION | BTAUDIO_CHANGED_PEER);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                portENTER_CRITICAL(&s_lock);
                s.connected = false;
                s.streaming = false;
                s.volume_notify = false;
                s.rssi_delta = BTAUDIO_UNKNOWN;
                s.text[BTAUDIO_TEXT_PEER_NAME][0] = 0;
                portEXIT_CRITICAL(&s_lock);
                set_text(BTAUDIO_TEXT_PEER, (const uint8_t *)"", 0);
                s.reset = true;
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                mark(BTAUDIO_CHANGED_CONNECTION | BTAUDIO_CHANGED_STREAM | BTAUDIO_CHANGED_PEER);
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
            // The delay to the speaker is the half of the ring that fills
            // before playback starts; tell the sender, which may use it to
            // keep video in step.
            uint32_t delay_ms = (s.capacity / 2) * 1000u / rate;
            portENTER_CRITICAL(&s_lock);
            s.sample_rate = rate;
            s.channels = channels;
            s.sbc_channel_mode = mcc->cie.sbc_info.ch_mode;
            s.sbc_block_length = mcc->cie.sbc_info.block_len;
            s.sbc_subbands = mcc->cie.sbc_info.num_subbands;
            s.sbc_allocation = mcc->cie.sbc_info.alloc_mthd;
            s.sbc_min_bitpool = mcc->cie.sbc_info.min_bitpool;
            s.sbc_max_bitpool = mcc->cie.sbc_info.max_bitpool;
            s.delay_ms = delay_ms;
            portEXIT_CRITICAL(&s_lock);
            esp_a2d_sink_set_delay_value((uint16_t)(delay_ms * 10));
            s.reset = true;
            mark(BTAUDIO_CHANGED_FORMAT | BTAUDIO_CHANGED_PEER);
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

// ---- GAP: pairing with no PIN to type, the sender's name and signal ----

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
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                set_cstr(BTAUDIO_TEXT_PEER_NAME, (const char *)param->read_rmt_name.rmt_name);
                mark(BTAUDIO_CHANGED_PEER);
            }
            break;
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
                portENTER_CRITICAL(&s_lock);
                bool changed = s.rssi_delta != param->read_rssi_delta.rssi_delta;
                s.rssi_delta = param->read_rssi_delta.rssi_delta;
                portEXIT_CRITICAL(&s_lock);
                if (changed) {
                    mark(BTAUDIO_CHANGED_PEER);
                }
            }
            break;
        default:
            break;
    }
}

// ---- the poll: position and signal, which senders don't all push ----

static void poll(void *arg) {
    (void)arg;
    btaudio_refresh();
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
    s.play_status = BTAUDIO_UNKNOWN;
    s.track_number = s.track_count = BTAUDIO_UNKNOWN;
    s.duration_ms = s.pos_ms = BTAUDIO_UNKNOWN;
    s.battery = BTAUDIO_UNKNOWN;
    s.rssi_delta = BTAUDIO_UNKNOWN;
    s.service = s.signal = s.roaming = s.phone_battery = BTAUDIO_UNKNOWN;
    s.speaker_gain = s.mic_gain = BTAUDIO_UNKNOWN;
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

#if BTAUDIO_HFP
    esp_hf_client_register_callback(hf_cb);
    if (esp_hf_client_init() != ESP_OK) {
        ESP_LOGW(TAG, "hands-free didn't start");
    }
#endif

    const esp_timer_create_args_t targs = {.callback = poll, .name = "btaudio"};
    if (esp_timer_create(&targs, &s_poll) == ESP_OK) {
        esp_timer_start_periodic(s_poll, POLL_US);
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
    if (s_poll) {
        esp_timer_stop(s_poll);
        esp_timer_delete(s_poll);
        s_poll = NULL;
    }
#if BTAUDIO_HFP
    esp_hf_client_deinit();
#endif
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
    free(s.cover_build);
    s.cover_build = NULL;
    free(s.cover);
    s.cover = NULL;
    s.cover_len = 0;
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

static int call_state(void) {
    if (s.ind_setup == 1) {
        return BTAUDIO_CALL_INCOMING;
    }
    if (s.ind_setup == 2) {
        return BTAUDIO_CALL_DIALING;
    }
    if (s.ind_setup == 3) {
        return BTAUDIO_CALL_ALERTING;
    }
    if (s.ind_call) {
        return s.held == 2 ? BTAUDIO_CALL_HELD : BTAUDIO_CALL_ACTIVE;
    }
    return BTAUDIO_CALL_IDLE;
}

void btaudio_status(btaudio_status_t *out) {
    uint32_t buffered = __atomic_load_n(&s.write_pos, __ATOMIC_ACQUIRE) - s.read_pos;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    out->connected = s.connected;
    out->streaming = s.streaming;
    out->playing = s.play_status == BTAUDIO_PLAY_PLAYING;
    out->sample_rate = s.sample_rate;
    out->volume = s.volume;
    out->buffered = buffered;
    out->capacity = s.capacity;
    out->underruns = s.underruns;
    out->overruns = s.overruns;
    out->dropped = s.dropped;
    out->repeated = s.repeated;
    out->received = s.received;

    out->play_status = s.play_status;
    out->track_number = s.track_number;
    out->track_count = s.track_count;
    out->duration_ms = s.duration_ms;
    int64_t pos = s.pos_ms;
    if (pos >= 0 && s.play_status == BTAUDIO_PLAY_PLAYING) {
        pos += (now - s.pos_at_us) / 1000;
        if (s.duration_ms > 0 && pos > s.duration_ms) {
            pos = s.duration_ms;
        }
    }
    out->position_ms = pos;
    out->battery = s.battery;
    out->avrcp_features = s.avrcp_features;
    out->avrcp_tg_flags = s.avrcp_tg_flags;
    out->cover_art = s.cover_connected;

    out->rssi_delta = s.connected ? s.rssi_delta : BTAUDIO_UNKNOWN;
    out->channels = s.channels;
    out->sbc_channel_mode = s.sbc_channel_mode;
    out->sbc_block_length = s.sbc_block_length;
    out->sbc_subbands = s.sbc_subbands;
    out->sbc_allocation = s.sbc_allocation;
    out->sbc_min_bitpool = s.sbc_min_bitpool;
    out->sbc_max_bitpool = s.sbc_max_bitpool;
    out->delay_ms = s.delay_ms;

    out->hf_connected = s.hf_connected;
    out->service = s.service;
    out->signal = s.signal;
    out->roaming = s.roaming;
    out->phone_battery = s.phone_battery;
    out->call = call_state();
    out->held = s.held;
    out->waiting = s.waiting;
    out->ringing = s.ringing;
    out->voice = s.voice;
    out->in_band_ring = s.in_band_ring;
    out->speaker_gain = s.speaker_gain;
    out->mic_gain = s.mic_gain;
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

size_t btaudio_cover_size(void) {
    portENTER_CRITICAL(&s_lock);
    size_t n = s.cover_len;
    portEXIT_CRITICAL(&s_lock);
    return n;
}

size_t btaudio_cover(uint8_t *dst, size_t size) {
    // Copy outside the lock (it's tens of KB): take the image away, copy,
    // and put it back unless a newer one arrived meanwhile.
    portENTER_CRITICAL(&s_lock);
    uint8_t *img = s.cover;
    size_t len = s.cover_len;
    s.cover = NULL;
    portEXIT_CRITICAL(&s_lock);
    if (img == NULL) {
        return 0;
    }
    size_t n = len <= size ? len : 0;
    if (n) {
        memcpy(dst, img, n);
    }
    portENTER_CRITICAL(&s_lock);
    bool newer = s.cover != NULL;
    if (!newer) {
        s.cover = img;
    }
    portEXIT_CRITICAL(&s_lock);
    if (newer) {
        free(img);
        return 0;
    }
    return n;
}

void btaudio_levels(uint16_t *left, uint16_t *right) {
    portENTER_CRITICAL(&s_lock);
    *left = s.peak_l;
    *right = s.peak_r;
    s.peak_l = 0;
    s.peak_r = 0;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t btaudio_key(uint8_t key, int state) {
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (state == 0 || state == 2) {
        err = esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    }
    if (err == ESP_OK && (state == 1 || state == 2)) {
        err = esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
    return err;
}

esp_err_t btaudio_command(btaudio_cmd_t cmd) {
    static const uint8_t keys[] = {
        [BTAUDIO_CMD_PLAY] = ESP_AVRC_PT_CMD_PLAY,
        [BTAUDIO_CMD_PAUSE] = ESP_AVRC_PT_CMD_PAUSE,
        [BTAUDIO_CMD_NEXT] = ESP_AVRC_PT_CMD_FORWARD,
        [BTAUDIO_CMD_PREVIOUS] = ESP_AVRC_PT_CMD_BACKWARD,
        [BTAUDIO_CMD_STOP] = ESP_AVRC_PT_CMD_STOP,
    };
    if ((unsigned)cmd >= sizeof(keys)) {
        return ESP_ERR_INVALID_ARG;
    }
    return btaudio_key(keys[cmd], 2);
}

esp_err_t btaudio_set_player(uint8_t attr, uint8_t value) {
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_avrc_ct_send_set_player_value_cmd(next_tl(), attr, value);
}

esp_err_t btaudio_refresh(void) {
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    bool avrc = s.avrc_connected;
    bool a2dp = s.connected;
    esp_bd_addr_t peer;
    memcpy(peer, s.peer, sizeof(peer));
    portEXIT_CRITICAL(&s_lock);
    if (avrc) {
        esp_avrc_ct_send_get_play_status_cmd(next_tl());
    }
    if (a2dp) {
        esp_bt_gap_read_rssi_delta(peer);
    }
    return ESP_OK;
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
    #if BTAUDIO_HFP
    if (s.hf_connected) {
        esp_hf_client_disconnect(peer);
    }
    #endif
    return connected ? esp_a2d_sink_disconnect(peer) : ESP_OK;
}

// ---- HFP calls ----

#if BTAUDIO_HFP
static esp_err_t hf_ready(void) {
    return s_started && s.hf_connected ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t btaudio_answer(void) {
    esp_err_t err = hf_ready();
    return err == ESP_OK ? esp_hf_client_answer_call() : err;
}

esp_err_t btaudio_hangup(void) {
    esp_err_t err = hf_ready();
    return err == ESP_OK ? esp_hf_client_reject_call() : err;
}

esp_err_t btaudio_dial(const char *number) {
    esp_err_t err = hf_ready();
    return err == ESP_OK ? esp_hf_client_dial(number) : err;
}

esp_err_t btaudio_voice(bool on) {
    esp_err_t err = hf_ready();
    if (err != ESP_OK) {
        return err;
    }
    return on ? esp_hf_client_start_voice_recognition() : esp_hf_client_stop_voice_recognition();
}

esp_err_t btaudio_dtmf(char digit) {
    esp_err_t err = hf_ready();
    return err == ESP_OK ? esp_hf_client_send_dtmf(digit) : err;
}
#else
esp_err_t btaudio_answer(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t btaudio_hangup(void) {
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t btaudio_dial(const char *number) {
    (void)number;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t btaudio_voice(bool on) {
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t btaudio_dtmf(char digit) {
    (void)digit;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
