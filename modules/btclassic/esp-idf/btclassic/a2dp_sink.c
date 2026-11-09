// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio sink's A2DP half on Bluedroid: the link, the stream's format,
// and the decoded audio into the ring. AVRCP, the other half, is in
// avrcp_ct.c. See include/btclassic.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "btc_bluedroid.h"

static const char *TAG = "btclassic";

#define MIN_RING_FRAMES 2048
#define DEFAULT_VOLUME 100
#define POLL_US 1000000             // play status and signal, once a second

btc_sink_state_t btc_sink;
static esp_timer_handle_t s_poll;
static esp_timer_handle_t s_retry;
static int16_t *s_ring_buf;
// Calling the last sender back: how many tries so far. A sender that still
// thinks the old link is up (the board lost power mid-stream) refuses the
// first calls until its own link times out.
static volatile bool s_calling_back;
static int s_tries;
// The program dropped the sender itself: don't call it back.
static volatile bool s_dropped;
// Bluedroid has finished taking A2DP down (ESP_A2D_DEINIT_SUCCESS).
static volatile bool s_deinit_done;
// Bluedroid has finished bringing A2DP up (ESP_A2D_INIT_SUCCESS).
static volatile bool s_init_done;
// The A2DP link's last reported state: connecting and disconnecting too.
static volatile esp_a2d_connection_state_t s_link;

#define CALL_BACK_TRIES 12
#define CALL_BACK_US 6000000        // apart, longer than a page times out: 72 s in all

#define s btc_sink

void btc_sink_mark(uint32_t bits) {
    BTC_LOCK();
    s.changes |= bits;
    BTC_UNLOCK();
}

bool btc_sink_peer(btc_addr_t out) {
    BTC_LOCK();
    bool connected = s.started && s.connected;
    memcpy(out, s.peer, sizeof(btc_addr_t));
    BTC_UNLOCK();
    return connected;
}

static bool is_peer(const uint8_t *bda) {
    return s.started && s.connected && memcmp(bda, s.peer, sizeof(btc_addr_t)) == 0;
}

void btc_sink_on_remote_name(const uint8_t *bda, const char *name) {
    if (is_peer(bda)) {
        btc_set_text(s.text[BTC_SINK_PEER_NAME], (const uint8_t *)name, strlen(name));
        btc_sink_mark(BTC_CHANGED_PEER);
    }
}

void btc_sink_on_rssi_delta(const uint8_t *bda, int delta) {
    if (!is_peer(bda)) {
        return;
    }
    BTC_LOCK();
    bool changed = s.rssi_delta != delta;
    s.rssi_delta = delta;
    BTC_UNLOCK();
    if (changed) {
        btc_sink_mark(BTC_CHANGED_PEER);
    }
}

// ---- the last sender, kept in NVS so a restart can call it back ----

#define NVS_NAMESPACE "btclassic"
#define NVS_KEY_LAST "last_a2dp"

static void save_last_sender(const uint8_t *bda) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_LAST, bda, sizeof(btc_addr_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_last_sender(btc_addr_t out) {
    size_t len = sizeof(btc_addr_t);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LAST, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(btc_addr_t);
}

static bool is_paired(const uint8_t *addr) {
    int n = btc_radio_paired(NULL, 0);
    if (n <= 0) {
        return false;
    }
    btc_addr_t *all = malloc(n * sizeof(btc_addr_t));
    if (all == NULL) {
        return false;
    }
    n = btc_radio_paired(all, n);
    bool found = false;
    for (int i = 0; i < n && !found; i++) {
        found = memcmp(all[i], addr, sizeof(btc_addr_t)) == 0;
    }
    free(all);
    return found;
}

// Call the last sender back, as a speaker does when it's switched on. Only a
// sender still paired: one that unpaired would just refuse.
static void reconnect_last_sender(void) {
    btc_addr_t last;
    if (load_last_sender(last) && is_paired(last)) {
        ESP_LOGI(TAG, "calling back the last sender (try %d)", s_tries + 1);
        s_calling_back = true;
        esp_a2d_sink_connect(last);
        // Try again later unless the link comes up first.
        if (s_retry) {
            esp_timer_start_once(s_retry, CALL_BACK_US);
        }
    }
}

static void retry(void *arg) {
    (void)arg;
    if (!s.started || !s_calling_back || s.connected) {
        return;
    }
    if (++s_tries >= CALL_BACK_TRIES) {
        s_calling_back = false;
        ESP_LOGI(TAG, "the last sender didn't answer");
        return;
    }
    reconnect_last_sender();
}

// A call back to a sender that still has the board's key reconnects without
// pairing. One that pairs instead has forgotten the board: it takes the new
// key and then refuses the audio link, so calling again only gets in the way
// of pairing it afresh.
void btc_sink_on_auth(const uint8_t *bda) {
    btc_addr_t last;
    if (s_calling_back && !s.connected && load_last_sender(last)
        && memcmp(last, bda, sizeof(last)) == 0) {
        s_calling_back = false;
        if (s_retry) {
            esp_timer_stop(s_retry);
        }
        ESP_LOGI(TAG, "the last sender has forgotten the board: not calling it back");
    }
}

// ---- A2DP: the link, the stream's format, and the audio ----

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_PROF_STATE_EVT:
            if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
                s_init_done = true;
                if (s.reconnect && !s_dropped) {
                    reconnect_last_sender();
                }
            } else if (param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS) {
                s_deinit_done = true;
            }
            break;
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            s_link = state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                uint8_t *a = param->conn_stat.remote_bda;
                s_calling_back = false;
                s_dropped = false;
                BTC_LOCK();
                memcpy(s.peer, a, sizeof(btc_addr_t));
                s.connected = true;
                BTC_UNLOCK();
                save_last_sender(a);
                esp_bt_gap_read_remote_name(a);
                // A phone also takes calls: the hands-free unit follows it.
                btc_hf_follow(a);
                // One sender at a time: out of searches until it leaves. The
                // board stays connectable, so a device that knows it still
                // reaches its other services (Serial, L2CAP, HID): Bluedroid
                // has one A2DP stream, and refuses a second sender itself.
                btc_radio_hold_unlisted(true);
                btc_sink_mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_PEER);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                bool was = s.connected;
                BTC_LOCK();
                s.connected = false;
                s.streaming = false;
                s.volume_notify = false;
                s.rssi_delta = BTC_UNKNOWN;
                s.text[BTC_SINK_PEER_NAME][0] = 0;
                BTC_UNLOCK();
                btc_ring_restart(&s.ring);
                btc_radio_hold_unlisted(false);
                btc_sink_mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_STREAM | BTC_CHANGED_PEER);
                // The link dropped (the sender went out of range, or its
                // Bluetooth went off): call it back for a minute, so it finds
                // the board when it returns, as a speaker does.
                if (was && s.reconnect && !s_dropped && s_retry) {
                    s_tries = 0;
                    s_calling_back = true;
                    esp_timer_start_once(s_retry, CALL_BACK_US);
                }
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            bool streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            BTC_LOCK();
            s.streaming = streaming;
            BTC_UNLOCK();
            btc_ring_restart(&s.ring);
            btc_sink_mark(BTC_CHANGED_STREAM);
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
            uint32_t delay_ms = (s.ring.capacity / 2) * 1000u / rate;
            BTC_LOCK();
            s.sample_rate = rate;
            s.channels = channels;
            s.sbc_channel_mode = mcc->cie.sbc_info.ch_mode;
            s.sbc_block_length = mcc->cie.sbc_info.block_len;
            s.sbc_subbands = mcc->cie.sbc_info.num_subbands;
            s.sbc_allocation = mcc->cie.sbc_info.alloc_mthd;
            s.sbc_min_bitpool = mcc->cie.sbc_info.min_bitpool;
            s.sbc_max_bitpool = mcc->cie.sbc_info.max_bitpool;
            s.delay_ms = delay_ms;
            BTC_UNLOCK();
            esp_a2d_sink_set_delay_value((uint16_t)(delay_ms * 10));
            btc_ring_restart(&s.ring);
            btc_sink_mark(BTC_CHANGED_FORMAT | BTC_CHANGED_PEER);
            ESP_LOGI(TAG, "stream: SBC, %u Hz, %u channel(s)", (unsigned)rate, channels);
            break;
        }
        default:
            break;
    }
}

// Decoded A2DP audio: 16-bit, interleaved when stereo.
static void a2d_data_cb(const uint8_t *buf, uint32_t len) {
    uint8_t channels = s.channels ? s.channels : 2;
    btc_ring_write(&s.ring, (const int16_t *)buf, len / (2u * channels), channels);
}

// ---- the poll: position and signal, which senders don't all push ----

static void poll(void *arg) {
    (void)arg;
    btc_sink_refresh();
}

// ---- start and stop ----

int btc_sink_start(size_t ring_frames, bool reconnect) {
    // Bluedroid runs one A2DP role at a time.
    if (s.started || !btc_radio_active() || btc_source_started()) {
        return BTC_ERR_STATE;
    }
    if (ring_frames < MIN_RING_FRAMES) {
        ring_frames = MIN_RING_FRAMES;
    }
    size_t bytes = ring_frames * 2 * sizeof(int16_t);
    int16_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return BTC_ERR_NOMEM;
    }
    memset(&s, 0, sizeof(s));
    s_ring_buf = buf;
    btc_ring_init(&s.ring, buf, ring_frames, 2);
    s.volume = DEFAULT_VOLUME;
    s.channels = 2;
    s.rssi_delta = BTC_UNKNOWN;
    s.reconnect = reconnect;
    s_calling_back = false;
    s_dropped = false;
    s_tries = 0;
    btc_avrcp_reset();
    s.started = true;

    const esp_timer_create_args_t rargs = {.callback = retry, .name = "btclassic_call"};
    esp_timer_create(&rargs, &s_retry);

    // AVRCP first, then A2DP, as Bluedroid asks.
    btc_avrcp_start();
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_sink_register_data_callback(a2d_data_cb);
    s_init_done = false;
    s_link = ESP_A2D_CONNECTION_STATE_DISCONNECTED;
    esp_err_t err = esp_a2d_sink_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP sink didn't start: %s", esp_err_to_name(err));
        btc_avrcp_stop();
        s.started = false;
        esp_timer_delete(s_retry);
        s_retry = NULL;
        btc_ring_init(&s.ring, NULL, 0, 2);
        heap_caps_free(s_ring_buf);
        s_ring_buf = NULL;
        return btc_err(err);
    }

    // Visible again as the settings say, now the sink's services are there:
    // the order Bluedroid's own examples use.
    btc_radio_hold_hidden(false);
    btc_radio_profile(BTC_PROFILE_SINK, true, 0);

    const esp_timer_create_args_t targs = {.callback = poll, .name = "btclassic"};
    if (esp_timer_create(&targs, &s_poll) == ESP_OK) {
        esp_timer_start_periodic(s_poll, POLL_US);
    }
    return BTC_OK;
}

void btc_sink_stop(void) {
    if (!s.started) {
        return;
    }
    s_calling_back = false;
    s_dropped = true;
    if (s_poll) {
        esp_timer_stop(s_poll);
        esp_timer_delete(s_poll);
        s_poll = NULL;
    }
    if (s_retry) {
        esp_timer_stop(s_retry);
        esp_timer_delete(s_retry);
        s_retry = NULL;
    }
    // Every A2DP and AVRCP link event runs through Bluedroid's A2DP state
    // machine, which deinit frees: one that arrives after it reads freed
    // memory and panics the board. So no link may be opening or closing when
    // A2DP goes. Hide, so no sender starts one, and let Bluedroid finish
    // coming up: it refuses a deinit until then, and comes up behind it.
    btc_radio_hold_hidden(true);
    for (int i = 0; i < 100 && !s_init_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Then let a link that's opening finish, close it, and wait until both
    // the A2DP and AVRCP links have closed and their events have drained.
    vTaskDelay(pdMS_TO_TICKS(200));
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 400 && (s_link == ESP_A2D_CONNECTION_STATE_CONNECTING
            || s_link == ESP_A2D_CONNECTION_STATE_DISCONNECTING); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!s.connected && !s.avrc_connected) {
            break;
        }
        btc_sink_disconnect();
        for (int i = 0; i < 400 && (s.connected || s.avrc_connected); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s.started = false;
    s_deinit_done = false;
    if (esp_a2d_sink_deinit() == ESP_OK) {
        for (int i = 0; i < 400 && !s_deinit_done; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    btc_avrcp_stop();
    btc_radio_hold_unlisted(false);
    btc_radio_hold_hidden(false);
    btc_radio_profile(BTC_PROFILE_SINK, false, 0);
    // Bluedroid's task may still be finishing a callback that writes the ring.
    vTaskDelay(pdMS_TO_TICKS(50));
    btc_ring_init(&s.ring, NULL, 0, 2);
    heap_caps_free(s_ring_buf);
    s_ring_buf = NULL;
    btc_avrcp_free_cover();
}

bool btc_sink_started(void) {
    return s.started;
}

// ---- reading ----

size_t btc_sink_read(int16_t *dst, size_t frames) {
    BTC_LOCK();
    int32_t level = s.volume;
    BTC_UNLOCK();
    return btc_ring_read(&s.ring, dst, frames, 2, level * level);  // about a dB taper
}

uint32_t btc_sink_changes(void) {
    BTC_LOCK();
    uint32_t c = s.changes;
    s.changes = 0;
    BTC_UNLOCK();
    return c;
}

void btc_sink_status(btc_sink_status_t *out) {
    int64_t now = esp_timer_get_time();
    BTC_LOCK();
    out->connected = s.connected;
    out->streaming = s.streaming;
    memcpy(out->peer, s.peer, sizeof(btc_addr_t));
    out->sample_rate = s.sample_rate;
    out->volume = s.volume;

    out->play_status = s.play_status;
    out->track_number = s.track_number;
    out->track_count = s.track_count;
    out->duration_ms = s.duration_ms;
    int64_t pos = s.pos_ms;
    if (pos >= 0 && s.play_status == BTC_PLAY_PLAYING) {
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

    out->rssi_delta = s.connected ? s.rssi_delta : BTC_UNKNOWN;
    out->channels = s.channels;
    out->sbc_channel_mode = s.sbc_channel_mode;
    out->sbc_block_length = s.sbc_block_length;
    out->sbc_subbands = s.sbc_subbands;
    out->sbc_allocation = s.sbc_allocation;
    out->sbc_min_bitpool = s.sbc_min_bitpool;
    out->sbc_max_bitpool = s.sbc_max_bitpool;
    out->delay_ms = s.delay_ms;
    BTC_UNLOCK();
}

void btc_sink_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&s.ring, out);
}

size_t btc_sink_text(btc_sink_text_t which, char *dst, size_t size) {
    if (which >= BTC_SINK_TEXT_COUNT) {
        return 0;
    }
    return btc_get_text(s.text[which], dst, size);
}

void btc_sink_levels(uint16_t *left, uint16_t *right) {
    // The ring holds its last peaks until audio arrives again, so once a
    // sender stops they would read as the last loud moment: silence instead,
    // after a quarter second with nothing new.
    static uint32_t seen;
    static int64_t moved_us;
    btc_ring_levels(&s.ring, left, right);
    uint32_t received = s.ring.received;
    int64_t now = esp_timer_get_time();
    if (received != seen) {
        seen = received;
        moved_us = now;
    } else if (now - moved_us > 250000) {
        *left = 0;
        *right = 0;
    }
}

int btc_sink_connect(const uint8_t *addr) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    btc_addr_t a;
    if (addr) {
        memcpy(a, addr, sizeof(a));
    } else if (!load_last_sender(a)) {
        return BTC_ERR_ARG;
    }
    return btc_err(esp_a2d_sink_connect(a));
}

int btc_sink_disconnect(void) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    btc_addr_t peer;
    bool connected = btc_sink_peer(peer);
    s_dropped = true;
    s_calling_back = false;
    return connected ? btc_err(esp_a2d_sink_disconnect(peer)) : BTC_OK;
}
