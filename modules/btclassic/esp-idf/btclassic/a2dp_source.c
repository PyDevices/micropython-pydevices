// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio source on Bluedroid: A2DP source with Bluedroid's own SBC
// encoder, and AVRCP both ways (the remote's keys and volume as target, the
// sink's volume as controller). Track metadata for the remote is answered in
// avrcp_tg.c. See include/btclassic.h.
//
// Bluedroid's encoder task pulls PCM at the stream's clock through the data
// callback; the program, or any C producer, pushes it in with
// btc_source_write(), which resamples to the stream's rate. The ring between
// them is the sink's ring, run the other way round: the writer stops at half
// full and a little, so the reader's clock correction never has to drop.

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

#define MIN_RING_FRAMES 4096
#define DEFAULT_VOLUME 100
#define KEY_QUEUE 16

#define NVS_NAMESPACE "btclassic"
#define NVS_KEY_LAST "last_sink"

static struct {
    bool started;
    btc_ring_t ring;
    int16_t *ring_buf;
    uint32_t changes;

    // A2DP
    bool connected;
    bool streaming;
    bool want_stream;       // start the stream when the link is up
    btc_addr_t peer;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t sbc_channel_mode, sbc_block_length, sbc_subbands, sbc_allocation;
    uint8_t sbc_min_bitpool, sbc_max_bitpool;
    uint16_t delay_ms;
    char text[BTC_SOURCE_TEXT_COUNT][BTC_TEXT_MAX];

    // AVRCP
    uint8_t volume;
    bool avrc_ct_connected;
    bool avrc_tg_connected;
    bool absolute_volume;   // the sink notifies and takes absolute volume
    uint8_t tl;
    uint16_t keys[KEY_QUEUE];
    uint8_t key_head, key_count;

    // What the target tells the remote.
    int track_number, track_count;
    int64_t duration_ms;
    int play_status;
    int64_t pos_ms;
    int64_t pos_at_us;
    uint32_t track_id;

    // The writer's resampler, the program's task only.
    uint32_t rs_in_rate, rs_out_rate;
    uint32_t rs_pos;        // Q16: where the next output frame sits, from rs_prev
    int16_t rs_prev[2];
} s;

static void mark(uint32_t bits) {
    BTC_LOCK();
    s.changes |= bits;
    BTC_UNLOCK();
}

static uint8_t next_tl(void) {
    BTC_LOCK();
    uint8_t tl = s.tl;
    s.tl = (s.tl + 1) & 0x0f;
    BTC_UNLOCK();
    return tl;
}

// ---- the last sink, kept so connect() with no address can call it ----

static void save_last_sink(const uint8_t *bda) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_LAST, bda, sizeof(btc_addr_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_last_sink(btc_addr_t out) {
    size_t len = sizeof(btc_addr_t);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_blob(h, NVS_KEY_LAST, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(btc_addr_t);
}

void btc_source_on_remote_name(const uint8_t *bda, const char *name) {
    if (s.started && s.connected && memcmp(bda, s.peer, sizeof(btc_addr_t)) == 0) {
        btc_set_text(s.text[BTC_SOURCE_PEER_NAME], (const uint8_t *)name, strlen(name));
        mark(BTC_CHANGED_PEER);
    }
}

// ---- A2DP ----

static void start_stream(void) {
    // Ask whether the stream can start; the acknowledgement starts it.
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
}

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
        case ESP_A2D_CONNECTION_STATE_EVT: {
            esp_a2d_connection_state_t state = param->conn_stat.state;
            if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                uint8_t *a = param->conn_stat.remote_bda;
                BTC_LOCK();
                memcpy(s.peer, a, sizeof(btc_addr_t));
                s.connected = true;
                BTC_UNLOCK();
                save_last_sink(a);
                esp_bt_gap_read_remote_name(a);
                ESP_LOGI(TAG, "source: connected");
                if (s.want_stream) {
                    start_stream();
                }
                // The stream's rate reads as 0 until now; say it's there.
                mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_FORMAT | BTC_CHANGED_PEER);
            } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                BTC_LOCK();
                s.connected = false;
                s.streaming = false;
                s.delay_ms = 0;
                s.text[BTC_SOURCE_PEER_NAME][0] = 0;
                BTC_UNLOCK();
                btc_ring_restart(&s.ring);
                mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_STREAM | BTC_CHANGED_FORMAT | BTC_CHANGED_PEER);
            }
            break;
        }
        case ESP_A2D_AUDIO_STATE_EVT: {
            bool streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
            BTC_LOCK();
            s.streaming = streaming;
            BTC_UNLOCK();
            mark(BTC_CHANGED_STREAM);
            break;
        }
        case ESP_A2D_AUDIO_CFG_EVT: {
            const esp_a2d_mcc_t *mcc = &param->audio_cfg.mcc;
            if (mcc->type != ESP_A2D_MCT_SBC) {
                break;
            }
            uint8_t sf = mcc->cie.sbc_info.samp_freq;
            uint32_t rate = sf & ESP_A2D_SBC_CIE_SF_48K ? 48000 : sf & ESP_A2D_SBC_CIE_SF_44K ? 44100
                : sf & ESP_A2D_SBC_CIE_SF_32K ? 32000 : 16000;
            #if ESP_IDF_VERSION_MAJOR < 6
            // ESP-IDF 5 feeds the encoder 44.1 kHz stereo PCM whatever the
            // stream; it resamples to the stream itself.
            rate = 44100;
            uint8_t channels = 2;
            #else
            uint8_t channels = (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
            #endif
            BTC_LOCK();
            s.sample_rate = rate;
            s.channels = channels;
            s.sbc_channel_mode = mcc->cie.sbc_info.ch_mode;
            s.sbc_block_length = mcc->cie.sbc_info.block_len;
            s.sbc_subbands = mcc->cie.sbc_info.num_subbands;
            s.sbc_allocation = mcc->cie.sbc_info.alloc_mthd;
            s.sbc_min_bitpool = mcc->cie.sbc_info.min_bitpool;
            s.sbc_max_bitpool = mcc->cie.sbc_info.max_bitpool;
            BTC_UNLOCK();
            mark(BTC_CHANGED_FORMAT | BTC_CHANGED_PEER);
            ESP_LOGI(TAG, "source stream: SBC, %u Hz PCM in", (unsigned)rate);
            break;
        }
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            if (param->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                ESP_LOGW(TAG, "source: media control %d refused", param->media_ctrl_stat.cmd);
                break;
            }
            if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && s.want_stream) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            }
            break;
        case ESP_A2D_REPORT_SNK_DELAY_VALUE_EVT:
            BTC_LOCK();
            s.delay_ms = param->a2d_report_delay_value_stat.delay_value / 10;
            BTC_UNLOCK();
            mark(BTC_CHANGED_PEER);
            break;
        default:
            break;
    }
}

// Bluedroid's encoder asks for `len` bytes of PCM at the stream's rate and
// channels. Always give it all of it: a short read stops this tick's
// encoding and the sink hears a gap anyway, so silence is the better gap.
static int32_t a2d_data_cb(uint8_t *buf, int32_t len) {
    if (buf == NULL || len <= 0) {
        return 0;
    }
    uint8_t ch = s.channels ? s.channels : 2;
    BTC_LOCK();
    int32_t gain = s.absolute_volume ? 16129 : (int32_t)s.volume * s.volume;
    BTC_UNLOCK();
    btc_ring_read(&s.ring, (int16_t *)buf, (size_t)len / (2u * ch), ch, gain);
    return len;
}

// ---- AVRCP controller: the sink's volume ----

static void send_volume(void) {
    if (s.avrc_ct_connected && s.absolute_volume) {
        esp_avrc_ct_send_set_absolute_volume_cmd(next_tl(), s.volume);
    }
}

static void register_volume(void) {
    esp_avrc_ct_send_register_notification_cmd(next_tl(), ESP_AVRC_RN_VOLUME_CHANGE, 0);
}

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            BTC_LOCK();
            s.avrc_ct_connected = param->conn_stat.connected;
            if (!param->conn_stat.connected) {
                s.absolute_volume = false;
            }
            BTC_UNLOCK();
            if (param->conn_stat.connected) {
                esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
            }
            mark(BTC_CHANGED_PEER);
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT: {
            esp_avrc_rn_evt_cap_mask_t caps = {.bits = param->get_rn_caps_rsp.evt_set.bits};
            bool abs = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &caps, ESP_AVRC_RN_VOLUME_CHANGE);
            BTC_LOCK();
            s.absolute_volume = abs;
            BTC_UNLOCK();
            if (abs) {
                register_volume();
                send_volume();
            }
            mark(BTC_CHANGED_PEER | BTC_CHANGED_VOLUME);
            break;
        }
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                BTC_LOCK();
                s.volume = param->change_ntf.event_parameter.volume & 0x7f;
                BTC_UNLOCK();
                mark(BTC_CHANGED_VOLUME);
                register_volume();
            }
            break;
        case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
            BTC_LOCK();
            s.volume = param->set_volume_rsp.volume & 0x7f;
            BTC_UNLOCK();
            mark(BTC_CHANGED_VOLUME);
            break;
        default:
            break;
    }
}

// ---- AVRCP target: the remote's keys, and its volume ----

static void push_key(uint8_t code, bool pressed) {
    BTC_LOCK();
    if (s.key_count == KEY_QUEUE) {     // full: lose the oldest
        s.key_head = (s.key_head + 1) % KEY_QUEUE;
        s.key_count--;
    }
    s.keys[(s.key_head + s.key_count) % KEY_QUEUE] = code | (pressed ? 0x100 : 0);
    s.key_count++;
    BTC_UNLOCK();
    mark(BTC_CHANGED_REMOTE);
}

static void avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    switch (event) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            BTC_LOCK();
            s.avrc_tg_connected = param->conn_stat.connected;
            BTC_UNLOCK();
            #if BTC_TG_METADATA
            btc_tg_metadata_reset();
            #endif
            mark(BTC_CHANGED_PEER);
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            push_key(param->psth_cmd.key_code & 0x7f, param->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED);
            break;
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            BTC_LOCK();
            s.volume = param->set_abs_vol.volume & 0x7f;
            BTC_UNLOCK();
            mark(BTC_CHANGED_VOLUME);
            break;
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
            if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                esp_avrc_rn_param_t rn = {0};
                rn.volume = s.volume;
                esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
            }
            break;
        default:
            break;
    }
}

static void avrcp_start(void) {
    esp_avrc_ct_register_callback(avrc_ct_cb);
    esp_avrc_ct_init();
    esp_avrc_tg_register_callback(avrc_tg_cb);
    esp_avrc_tg_init();
    // Every key AVRCP lets a target take: the app decides what each does.
    esp_avrc_psth_bit_mask_t keys = {0};
    esp_avrc_tg_get_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_ALLOWED_CMD, &keys);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &keys);
    esp_avrc_rn_evt_cap_mask_t evts = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evts, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evts);
}

static void avrcp_stop(void) {
    esp_avrc_tg_deinit();
    esp_avrc_ct_deinit();
}

// ---- start and stop ----

int btc_source_start(size_t ring_frames) {
    if (s.started || !btc_radio_active() || btc_sink_started()) {
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
    s.ring_buf = buf;
    btc_ring_init(&s.ring, buf, ring_frames, 2);
    s.volume = DEFAULT_VOLUME;
    s.channels = 2;
    s.want_stream = true;
    s.play_status = BTC_PLAY_STOPPED;
    s.track_number = s.track_count = BTC_UNKNOWN;
    s.duration_ms = BTC_UNKNOWN;
    s.pos_ms = BTC_UNKNOWN;
    s.started = true;

    // AVRCP first, then A2DP, as Bluedroid asks.
    avrcp_start();
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data_cb);
    esp_err_t err = esp_a2d_source_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source didn't start: %s", esp_err_to_name(err));
        avrcp_stop();
        s.started = false;
        btc_ring_init(&s.ring, NULL, 0, 2);
        heap_caps_free(buf);
        s.ring_buf = NULL;
        return btc_err(err);
    }
    btc_radio_profile(BTC_PROFILE_SOURCE, true, 0);
    return BTC_OK;
}

void btc_source_stop(void) {
    if (!s.started) {
        return;
    }
    s.want_stream = false;
    if (s.connected) {
        btc_source_disconnect();
        for (int i = 0; i < 100 && s.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s.started = false;
    esp_a2d_source_deinit();
    avrcp_stop();
    btc_radio_profile(BTC_PROFILE_SOURCE, false, 0);
    // Bluedroid's encoder task may still be in the data callback.
    vTaskDelay(pdMS_TO_TICKS(50));
    btc_ring_init(&s.ring, NULL, 0, 2);
    heap_caps_free(s.ring_buf);
    s.ring_buf = NULL;
}

bool btc_source_started(void) {
    return s.started;
}

int btc_source_connect(const uint8_t *addr) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    btc_addr_t a;
    if (addr) {
        memcpy(a, addr, sizeof(a));
    } else if (!load_last_sink(a)) {
        return BTC_ERR_ARG;
    }
    s.want_stream = true;
    return btc_err(esp_a2d_source_connect(a));
}

int btc_source_disconnect(void) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    if (!s.connected) {
        return BTC_OK;
    }
    btc_addr_t peer;
    BTC_LOCK();
    memcpy(peer, s.peer, sizeof(peer));
    BTC_UNLOCK();
    return btc_err(esp_a2d_source_disconnect(peer));
}

int btc_source_stream(bool on) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    s.want_stream = on;
    if (!s.connected) {
        return BTC_OK;     // it starts when the link comes up
    }
    if (on) {
        if (!s.streaming) {
            start_stream();
        }
        return BTC_OK;
    }
    return s.streaming ? btc_err(esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND)) : BTC_OK;
}

// ---- writing ----

// How many frames the writer may add now: up to half the ring and an
// eighth of that, the reader's target plus half its slack.
static uint32_t write_room(void) {
    btc_ring_t *r = &s.ring;
    if (r->buf == NULL) {
        return 0;
    }
    // A restart waits for the reader, which drops what's there when the
    // stream starts again; until then the ring stays full and writes wait.
    uint32_t used = __atomic_load_n(&r->write_pos, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->read_pos, __ATOMIC_ACQUIRE);
    uint32_t cap = r->limit / 2 + r->limit / 16;
    return used < cap ? cap - used : 0;
}

#define RS_CHUNK 128

size_t btc_source_write(const int16_t *pcm, size_t frames, uint32_t rate, uint8_t channels) {
    if (!s.started || frames == 0 || (channels != 1 && channels != 2)) {
        return 0;
    }
    uint32_t out_rate = s.sample_rate ? s.sample_rate : 44100;
    uint32_t room = write_room();
    if (rate == out_rate) {
        uint32_t n = frames < room ? (uint32_t)frames : room;
        s.rs_in_rate = 0;
        return n ? btc_ring_write(&s.ring, pcm, n, channels) : 0;
    }
    // Linear interpolation to the stream's rate. Position 0 is the last
    // frame of the previous write, 1 is pcm[0].
    if (s.rs_in_rate != rate || s.rs_out_rate != out_rate) {
        s.rs_in_rate = rate;
        s.rs_out_rate = out_rate;
        s.rs_pos = 1u << 16;
        s.rs_prev[0] = pcm[0];
        s.rs_prev[1] = pcm[channels - 1];
    }
    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / out_rate);
    int16_t out[RS_CHUNK * 2];
    uint32_t pos = s.rs_pos;
    size_t done = 0;    // output frames made
    for (;;) {
        uint32_t n = 0;
        while (n < RS_CHUNK && done + n < room) {
            uint32_t i = pos >> 16;
            if (i + 1 > frames) {
                break;
            }
            int32_t frac = pos & 0xffff;
            for (int c = 0; c < 2; c++) {
                int32_t a = i == 0 ? s.rs_prev[c] : pcm[(i - 1) * channels + (channels == 2 ? c : 0)];
                int32_t b = pcm[i * channels + (channels == 2 ? c : 0)];
                out[n * 2 + c] = (int16_t)(a + (((b - a) * frac) >> 16));
            }
            pos += step;
            n++;
        }
        if (n == 0) {
            break;
        }
        btc_ring_write(&s.ring, out, n, 2);
        done += n;
    }
    // Input frames wholly behind the position are used up.
    uint32_t used = pos >> 16;
    if (used > frames) {
        used = (uint32_t)frames;
    }
    if (used > 0) {
        s.rs_prev[0] = pcm[(used - 1) * channels];
        s.rs_prev[1] = pcm[(used - 1) * channels + channels - 1];
    }
    s.rs_pos = pos - (used << 16);
    return used;
}

// ---- state ----

static int64_t position_now(int64_t now_us);

uint32_t btc_source_changes(void) {
    BTC_LOCK();
    uint32_t c = s.changes;
    s.changes = 0;
    BTC_UNLOCK();
    return c;
}

void btc_source_status(btc_source_status_t *out) {
    BTC_LOCK();
    out->connected = s.connected;
    out->streaming = s.streaming;
    memcpy(out->peer, s.peer, sizeof(btc_addr_t));
    out->sample_rate = s.connected ? s.sample_rate : 0;
    out->channels = s.channels;
    out->volume = s.volume;
    out->absolute_volume = s.absolute_volume;
    out->avrcp = s.avrc_ct_connected || s.avrc_tg_connected;
    out->sbc_channel_mode = s.sbc_channel_mode;
    out->sbc_block_length = s.sbc_block_length;
    out->sbc_subbands = s.sbc_subbands;
    out->sbc_allocation = s.sbc_allocation;
    out->sbc_min_bitpool = s.sbc_min_bitpool;
    out->sbc_max_bitpool = s.sbc_max_bitpool;
    out->delay_ms = s.delay_ms;
    out->metadata = BTC_TG_METADATA;
    out->play_status = s.play_status;
    out->position_ms = position_now(esp_timer_get_time());
    BTC_UNLOCK();
}

void btc_source_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&s.ring, out);
}

size_t btc_source_text(btc_source_text_t which, char *dst, size_t size) {
    if (which >= BTC_SOURCE_TEXT_COUNT) {
        return 0;
    }
    return btc_get_text(s.text[which], dst, size);
}

void btc_source_set_volume(uint8_t volume) {
    BTC_LOCK();
    s.volume = volume & 0x7f;
    BTC_UNLOCK();
    send_volume();
    if (s.avrc_tg_connected) {
        // A remote that registered for our volume hears of it.
        esp_avrc_rn_param_t rn = {0};
        rn.volume = s.volume;
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
    }
    mark(BTC_CHANGED_VOLUME);
}

int btc_source_key(void) {
    int key = -1;
    BTC_LOCK();
    if (s.key_count) {
        key = s.keys[s.key_head];
        s.key_head = (s.key_head + 1) % KEY_QUEUE;
        s.key_count--;
    }
    BTC_UNLOCK();
    return key;
}

// ---- what the target tells the remote ----

static int64_t position_now(int64_t now_us) {
    int64_t pos = s.pos_ms;
    if (pos >= 0 && s.play_status == BTC_PLAY_PLAYING) {
        pos += (now_us - s.pos_at_us) / 1000;
        if (s.duration_ms > 0 && pos > s.duration_ms) {
            pos = s.duration_ms;
        }
    }
    return pos;
}

static bool set_field(btc_source_text_t which, const char *text) {
    if (text == NULL) {
        return false;
    }
    char old[BTC_TEXT_MAX];
    btc_get_text(s.text[which], old, sizeof(old));
    btc_set_text(s.text[which], (const uint8_t *)text, strlen(text));
    return strncmp(old, text, BTC_TEXT_MAX - 1) != 0;
}

int btc_source_set_track(const char *title, const char *artist, const char *album,
    const char *genre, int track_number, int track_count, int64_t duration_ms) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    bool new_track = set_field(BTC_SOURCE_TITLE, title);
    set_field(BTC_SOURCE_ARTIST, artist);
    set_field(BTC_SOURCE_ALBUM, album);
    set_field(BTC_SOURCE_GENRE, genre);
    BTC_LOCK();
    s.track_number = track_number;
    s.track_count = track_count;
    s.duration_ms = duration_ms;
    if (new_track) {
        s.track_id++;
        s.pos_ms = 0;
        s.pos_at_us = esp_timer_get_time();
    }
    BTC_UNLOCK();
    #if BTC_TG_METADATA
    btc_tg_metadata_changed(ESP_AVRC_RN_TRACK_CHANGE);
    #endif
    return BTC_OK;
}

int btc_source_set_play_status(int status, int64_t position_ms) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    int64_t now = esp_timer_get_time();
    BTC_LOCK();
    if (status < 0) {
        status = s.play_status;
    }
    bool changed = s.play_status != status;
    int64_t pos = position_ms >= 0 ? position_ms : position_now(now);
    s.pos_ms = pos;
    s.pos_at_us = now;
    s.play_status = status;
    BTC_UNLOCK();
    #if BTC_TG_METADATA
    if (changed) {
        btc_tg_metadata_changed(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
    }
    if (position_ms >= 0) {
        btc_tg_metadata_changed(ESP_AVRC_RN_PLAY_POS_CHANGED);
    }
    #else
    (void)changed;
    #endif
    return BTC_OK;
}

bool btc_source_tg_active(void) {
    return s.started;
}

void btc_source_tg_track(btc_tg_track_t *out) {
    int64_t now = esp_timer_get_time();
    btc_get_text(s.text[BTC_SOURCE_TITLE], out->title, sizeof(out->title));
    btc_get_text(s.text[BTC_SOURCE_ARTIST], out->artist, sizeof(out->artist));
    btc_get_text(s.text[BTC_SOURCE_ALBUM], out->album, sizeof(out->album));
    btc_get_text(s.text[BTC_SOURCE_GENRE], out->genre, sizeof(out->genre));
    BTC_LOCK();
    out->track_number = s.track_number;
    out->track_count = s.track_count;
    out->duration_ms = s.duration_ms;
    out->play_status = s.play_status;
    out->position_ms = position_now(now);
    out->track_id = s.track_id;
    BTC_UNLOCK();
}
