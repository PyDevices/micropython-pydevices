// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio sink's AVRCP half on Bluedroid: the controller reads what the
// sender plays, cover art included, and drives its transport and settings;
// the target holds the volume slider shared with the sender. See
// include/btclassic.h.

#include <stdlib.h>
#include <string.h>

#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "btc_bluedroid.h"

#define COVER_MAX (256 * 1024)      // a thumbnail is a few tens of KB
#define AVRCP_UNKNOWN_MS 0xffffffffu

#define s btc_sink
#define mark btc_sink_mark

static uint8_t next_tl(void) {
    BTC_LOCK();
    uint8_t tl = s.tl;
    s.tl = (s.tl + 1) & ESP_AVRC_TRANS_LABEL_MAX;
    BTC_UNLOCK();
    return tl;
}

// A decimal attribute (track number, count, playing time), or UNKNOWN.
static int64_t parse_number(const uint8_t *src, int len) {
    int64_t v = 0;
    if (len <= 0) {
        return BTC_UNKNOWN;
    }
    for (int i = 0; i < len; i++) {
        if (src[i] < '0' || src[i] > '9') {
            return BTC_UNKNOWN;
        }
        v = v * 10 + (src[i] - '0');
    }
    return v;
}

static void set_position(uint32_t ms) {
    BTC_LOCK();
    s.pos_ms = ms == AVRCP_UNKNOWN_MS ? BTC_UNKNOWN : (int64_t)ms;
    s.pos_at_us = esp_timer_get_time();
    BTC_UNLOCK();
}

// ---- cover art ----

static void cover_set(uint8_t *jpeg, size_t len) {
    BTC_LOCK();
    uint8_t *old = s.cover;
    s.cover = jpeg;
    s.cover_len = jpeg ? len : 0;
    BTC_UNLOCK();
    free(old);
    mark(BTC_CHANGED_COVER);
}

static void cover_clear(void) {
    BTC_LOCK();
    bool had = s.cover != NULL;
    BTC_UNLOCK();
    if (had) {
        cover_set(NULL, 0);
    }
}

void btc_avrcp_free_cover(void) {
    free(s.cover_build);
    s.cover_build = NULL;
    s.cover_build_len = 0;
    BTC_LOCK();
    uint8_t *old = s.cover;
    s.cover = NULL;
    s.cover_len = 0;
    BTC_UNLOCK();
    free(old);
}

#if BTC_COVER
// Fetch the thumbnail for the handle the metadata named, one at a time and
// once per image: senders give each image its own handle.
static void cover_fetch(void) {
    if (!s.cover_connected || s.cover_getting || !s.cover_has_handle) {
        return;
    }
    if (memcmp(s.cover_handle, s.cover_fetched, BTC_HANDLE_LEN) == 0) {
        return;
    }
    memcpy(s.cover_fetched, s.cover_handle, BTC_HANDLE_LEN);
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

size_t btc_sink_cover_size(void) {
    BTC_LOCK();
    size_t n = s.cover_len;
    BTC_UNLOCK();
    return n;
}

size_t btc_sink_cover(uint8_t *dst, size_t size) {
    // Copy outside the lock (it's tens of KB): take the image away, copy,
    // and put it back unless a newer one arrived meanwhile.
    BTC_LOCK();
    uint8_t *img = s.cover;
    size_t len = s.cover_len;
    s.cover = NULL;
    BTC_UNLOCK();
    if (img == NULL) {
        return 0;
    }
    size_t n = len <= size ? len : 0;
    if (n) {
        memcpy(dst, img, n);
    }
    BTC_LOCK();
    bool newer = s.cover != NULL;
    if (!newer) {
        s.cover = img;
    }
    BTC_UNLOCK();
    if (newer) {
        free(img);
        return 0;
    }
    return n;
}

// ---- the controller: what the sender is playing, and its transport ----

static bool peer_supports(uint8_t event_id) {
    esp_avrc_rn_evt_cap_mask_t caps = {.bits = s.peer_events};
    return esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps, event_id);
}

static void request_metadata(void) {
    uint8_t mask = ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM
        | ESP_AVRC_MD_ATTR_TRACK_NUM | ESP_AVRC_MD_ATTR_NUM_TRACKS | ESP_AVRC_MD_ATTR_GENRE
        | ESP_AVRC_MD_ATTR_PLAYING_TIME;
    #if BTC_COVER
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
    BTC_LOCK();
    s.text[BTC_SINK_TITLE][0] = 0;
    s.text[BTC_SINK_ARTIST][0] = 0;
    s.text[BTC_SINK_ALBUM][0] = 0;
    s.text[BTC_SINK_GENRE][0] = 0;
    s.track_number = BTC_UNKNOWN;
    s.track_count = BTC_UNKNOWN;
    s.duration_ms = BTC_UNKNOWN;
    s.pos_ms = BTC_UNKNOWN;
    s.cover_has_handle = false;
    BTC_UNLOCK();
}

void btc_avrcp_reset(void) {
    BTC_LOCK();
    s.avrc_connected = false;
    s.peer_events = 0;
    s.play_status = BTC_UNKNOWN;
    s.battery = BTC_UNKNOWN;
    s.avrcp_features = 0;
    s.avrcp_tg_flags = 0;
    s.cover_connected = false;
    s.cover_getting = false;
    memset(s.cover_fetched, 0, BTC_HANDLE_LEN);
    BTC_UNLOCK();
    clear_track();
}

static void set_play_status(int status) {
    BTC_LOCK();
    bool changed = s.play_status != status;
    // Freeze the position where it is when playback stops moving it.
    if (changed && s.play_status == BTC_PLAY_PLAYING && s.pos_ms >= 0) {
        int64_t now = esp_timer_get_time();
        s.pos_ms += (now - s.pos_at_us) / 1000;
        s.pos_at_us = now;
    } else if (changed && status == BTC_PLAY_PLAYING) {
        s.pos_at_us = esp_timer_get_time();
    }
    s.play_status = status;
    BTC_UNLOCK();
    if (changed) {
        mark(BTC_CHANGED_PLAYING);
    }
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (param->conn_stat.connected) {
                BTC_LOCK();
                s.avrc_connected = true;
                BTC_UNLOCK();
                esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
            } else {
                btc_avrcp_reset();
                cover_clear();
                mark(BTC_CHANGED_TRACK | BTC_CHANGED_PLAYING | BTC_CHANGED_PEER);
            }
            break;
        case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
            BTC_LOCK();
            s.avrcp_features = param->rmt_feats.feat_mask;
            s.avrcp_tg_flags = param->rmt_feats.tg_feat_flag;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PEER);
            #if BTC_COVER
            if ((param->rmt_feats.tg_feat_flag & ESP_AVRC_FEAT_FLAG_TG_COVER_ART) && !s.cover_connected) {
                esp_avrc_ct_cover_art_connect(ESP_AVRC_CA_MTU_MAX);
            }
            #endif
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
            BTC_LOCK();
            s.peer_events = param->get_rn_caps_rsp.evt_set.bits;
            BTC_UNLOCK();
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
                    btc_set_text(s.text[BTC_SINK_TITLE], text, len);
                    break;
                case ESP_AVRC_MD_ATTR_ARTIST:
                    btc_set_text(s.text[BTC_SINK_ARTIST], text, len);
                    break;
                case ESP_AVRC_MD_ATTR_ALBUM:
                    btc_set_text(s.text[BTC_SINK_ALBUM], text, len);
                    break;
                case ESP_AVRC_MD_ATTR_GENRE:
                    btc_set_text(s.text[BTC_SINK_GENRE], text, len);
                    break;
                case ESP_AVRC_MD_ATTR_TRACK_NUM: {
                    int64_t v = parse_number(text, len);
                    BTC_LOCK();
                    s.track_number = v > 0 ? (int)v : BTC_UNKNOWN;
                    BTC_UNLOCK();
                    break;
                }
                case ESP_AVRC_MD_ATTR_NUM_TRACKS: {
                    int64_t v = parse_number(text, len);
                    BTC_LOCK();
                    s.track_count = v > 0 ? (int)v : BTC_UNKNOWN;
                    BTC_UNLOCK();
                    break;
                }
                case ESP_AVRC_MD_ATTR_PLAYING_TIME: {
                    int64_t v = parse_number(text, len);
                    BTC_LOCK();
                    s.duration_ms = v > 0 ? v : BTC_UNKNOWN;
                    BTC_UNLOCK();
                    break;
                }
                #if BTC_COVER
                case ESP_AVRC_MD_ATTR_COVER_ART:
                    if (len == BTC_HANDLE_LEN) {
                        memcpy(s.cover_handle, text, BTC_HANDLE_LEN);
                        s.cover_has_handle = true;
                        cover_fetch();
                    } else {
                        // The track has no image.
                        s.cover_has_handle = false;
                        memset(s.cover_fetched, 0, BTC_HANDLE_LEN);
                        cover_clear();
                    }
                    return;
                #endif
                default:
                    return;
            }
            mark(BTC_CHANGED_TRACK);
            break;
        }
        case ESP_AVRC_CT_PLAY_STATUS_RSP_EVT: {
            uint32_t length = param->play_status_rsp.song_length;
            BTC_LOCK();
            if (length != AVRCP_UNKNOWN_MS && length != 0) {
                s.duration_ms = length;
            }
            BTC_UNLOCK();
            set_position(param->play_status_rsp.song_position);
            set_play_status(param->play_status_rsp.play_status);
            mark(BTC_CHANGED_POSITION);
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT: {
            uint8_t id = param->change_ntf.event_id;
            switch (id) {
                case ESP_AVRC_RN_TRACK_CHANGE:
                    clear_track();
                    mark(BTC_CHANGED_TRACK);
                    request_metadata();
                    esp_avrc_ct_send_get_play_status_cmd(next_tl());
                    break;
                case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
                    set_play_status(param->change_ntf.event_parameter.playback);
                    esp_avrc_ct_send_get_play_status_cmd(next_tl());
                    break;
                case ESP_AVRC_RN_PLAY_POS_CHANGED:
                    set_position(param->change_ntf.event_parameter.play_pos);
                    mark(BTC_CHANGED_POSITION);
                    break;
                case ESP_AVRC_RN_BATTERY_STATUS_CHANGE:
                    BTC_LOCK();
                    s.battery = param->change_ntf.event_parameter.batt;
                    BTC_UNLOCK();
                    mark(BTC_CHANGED_PEER);
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
        #if BTC_COVER
        case ESP_AVRC_CT_COVER_ART_STATE_EVT:
            s.cover_connected = param->cover_art_state.state == ESP_AVRC_COVER_ART_CONNECTED;
            s.cover_getting = false;
            mark(BTC_CHANGED_PEER);
            if (s.cover_connected) {
                // Ask again: the handle may have come before the channel.
                memset(s.cover_fetched, 0, BTC_HANDLE_LEN);
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

// ---- the target: the sender's volume slider ----

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            BTC_LOCK();
            s.volume = param->set_abs_vol.volume & 0x7f;
            BTC_UNLOCK();
            mark(BTC_CHANGED_VOLUME);
            break;
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                esp_avrc_rn_param_t rn = {0};
                BTC_LOCK();
                s.volume_notify = true;
                rn.volume = s.volume;
                BTC_UNLOCK();
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
            }
            break;
        default:
            break;
    }
}

void btc_avrcp_start(void) {
    esp_avrc_ct_register_callback(avrc_ct_cb);
    esp_avrc_ct_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_tg_init();
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);
}

void btc_avrcp_stop(void) {
    esp_avrc_tg_deinit();
    esp_avrc_ct_deinit();
    btc_avrcp_reset();
}

// ---- commands to the sender ----

int btc_sink_key(uint8_t key, int state) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    esp_err_t err = ESP_OK;
    if (state == 0 || state == 2) {
        err = esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_PRESSED);
    }
    if (err == ESP_OK && (state == 1 || state == 2)) {
        err = esp_avrc_ct_send_passthrough_cmd(next_tl(), key, ESP_AVRC_PT_CMD_STATE_RELEASED);
    }
    return btc_err(err);
}

int btc_sink_set_player(uint8_t attr, uint8_t value) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    return btc_err(esp_avrc_ct_send_set_player_value_cmd(next_tl(), attr, value));
}

int btc_sink_refresh(void) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    BTC_LOCK();
    bool avrc = s.avrc_connected;
    bool a2dp = s.connected;
    esp_bd_addr_t peer;
    memcpy(peer, s.peer, sizeof(peer));
    BTC_UNLOCK();
    if (avrc) {
        esp_avrc_ct_send_get_play_status_cmd(next_tl());
    }
    if (a2dp) {
        esp_bt_gap_read_rssi_delta(peer);
    }
    return BTC_OK;
}

void btc_sink_set_volume(uint8_t volume) {
    volume &= 0x7f;
    BTC_LOCK();
    s.volume = volume;
    bool notify = s.volume_notify;
    s.volume_notify = false;
    BTC_UNLOCK();
    if (s.started && notify) {
        esp_avrc_rn_param_t rn = {0};
        rn.volume = volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
    }
    mark(BTC_CHANGED_VOLUME);
}
