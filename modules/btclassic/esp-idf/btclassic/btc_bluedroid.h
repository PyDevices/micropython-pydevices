// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// What the Bluedroid backend's files share with each other, and nobody
// else sees. Bluedroid calls every callback from its own task; the state
// callbacks and callers share sits under btc_lock.

#pragma once

#include "freertos/FreeRTOS.h"

#include "esp_err.h"
#include "sdkconfig.h"

#include "btclassic.h"
#include "ring.h"

#if CONFIG_BT_HFP_CLIENT_ENABLE
#define BTC_HFP 1
#else
#define BTC_HFP 0
#endif

// Call audio over HCI, with Bluedroid's own CVSD and mSBC codecs: the PCM
// callbacks hand over 16-bit mono at 8 or 16 kHz.
#if BTC_HFP && CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI && !CONFIG_BT_HFP_USE_EXTERNAL_CODEC
#define BTC_CALL_AUDIO 1
#else
#define BTC_CALL_AUDIO 0
#endif

#if CONFIG_BT_AVRCP_CT_COVER_ART_ENABLED
#define BTC_COVER 1
#else
#define BTC_COVER 0
#endif

// The AVRCP target answers a remote's track and play status queries, which
// Bluedroid's public API can't (avrcp_tg.c). The build defines it where the
// link-time hooks it needs are known to fit.
#ifndef BTC_TG_METADATA
#define BTC_TG_METADATA 0
#endif

#define BTC_TEXT_MAX 128
#define BTC_HANDLE_LEN 7    // AVRCP image handles are 7 digits

extern portMUX_TYPE btc_lock;
#define BTC_LOCK() portENTER_CRITICAL(&btc_lock)
#define BTC_UNLOCK() portEXIT_CRITICAL(&btc_lock)

// An ESP-IDF error as a BTC_ERR_* result.
int btc_err(esp_err_t err);

// Copy at most BTC_TEXT_MAX - 1 bytes into a text field, under the lock.
void btc_set_text(char *field, const uint8_t *src, size_t len);
size_t btc_get_text(const char *field, char *dst, size_t size);

// ---- the radio, for the profiles ----

// A profile that takes one device at a time hides the board while it has
// one, and shows it again (as the settings say) when it leaves.
void btc_radio_hold_hidden(bool hidden);

// Set radio changes() bits, from another part of the backend.
void btc_radio_mark(uint32_t bits);

// ---- the sink: A2DP (a2dp_sink.c) and AVRCP (avrcp_ct.c) share this ----

typedef struct {
    bool started;
    bool reconnect;
    btc_ring_t ring;
    uint32_t changes;

    // A2DP
    bool connected;
    bool streaming;
    btc_addr_t peer;
    uint8_t channels;
    uint32_t sample_rate;
    uint8_t volume;
    char text[BTC_SINK_TEXT_COUNT][BTC_TEXT_MAX];
    int rssi_delta;
    uint8_t sbc_channel_mode, sbc_block_length, sbc_subbands, sbc_allocation;
    uint8_t sbc_min_bitpool, sbc_max_bitpool;
    uint16_t delay_ms;

    // AVRCP
    bool avrc_connected;
    bool volume_notify;         // the sender asked to hear of volume changes
    uint16_t peer_events;       // AVRCP notifications the sender supports
    uint8_t tl;                 // AVRCP transaction label
    int play_status;
    int track_number, track_count;
    int64_t duration_ms;
    int64_t pos_ms;             // last reported position
    int64_t pos_at_us;          // when it was reported
    int battery;
    uint32_t avrcp_features;
    uint16_t avrcp_tg_flags;

    // Cover art: the image handle the metadata named, the one fetched, and
    // the JPEG assembled from the data events.
    bool cover_connected;
    bool cover_getting;
    uint8_t cover_handle[BTC_HANDLE_LEN];
    bool cover_has_handle;
    uint8_t cover_fetched[BTC_HANDLE_LEN];
    uint8_t *cover;             // the finished image, under the lock
    size_t cover_len;
    uint8_t *cover_build;       // being assembled, Bluedroid's task only
    size_t cover_build_len;
} btc_sink_state_t;

extern btc_sink_state_t btc_sink;

void btc_sink_mark(uint32_t bits);

// The GAP callback hands these on to the sink.
void btc_sink_on_remote_name(const uint8_t *bda, const char *name);
void btc_sink_on_rssi_delta(const uint8_t *bda, int delta);
void btc_sink_on_auth(const uint8_t *bda);

// avrcp_ct.c: AVRCP controller and target, started before A2DP.
void btc_avrcp_start(void);
void btc_avrcp_stop(void);
void btc_avrcp_reset(void);     // forget the player: on start and disconnect
void btc_avrcp_free_cover(void);

// The sink's sender while connected, for the hands-free unit.
bool btc_sink_peer(btc_addr_t out);

// ---- the hands-free unit (hfp_hf.c) ----

// The sink connected to `bda`: the hands-free link follows, if started.
void btc_hf_follow(const uint8_t *bda);

// ---- discovery (inquiry.c) ----

// Every GAP event, from radio.c's callback: inquiry results, and the names
// the source asked for. `event` and `param` are Bluedroid's GAP types.
void btc_inquiry_on_gap(int event, void *param);
void btc_inquiry_stop(void);

// ---- the source (a2dp_source.c) and its AVRCP target metadata (avrcp_tg.c) ----

void btc_source_on_remote_name(const uint8_t *bda, const char *name);

// What avrcp_tg.c answers the remote with, under the lock.
typedef struct {
    char title[BTC_TEXT_MAX], artist[BTC_TEXT_MAX], album[BTC_TEXT_MAX], genre[BTC_TEXT_MAX];
    int track_number, track_count;
    int64_t duration_ms;
    int play_status;
    int64_t position_ms;    // now, counted on while playing
    uint32_t track_id;      // goes up with each new track
} btc_tg_track_t;

bool btc_source_tg_active(void);
void btc_source_tg_track(btc_tg_track_t *out);

// avrcp_tg.c, when BTC_TG_METADATA: the remote registered for these events,
// and they changed. Safe from any task.
#if BTC_TG_METADATA
void btc_tg_metadata_reset(void);
void btc_tg_metadata_changed(uint8_t event_id);
#endif
