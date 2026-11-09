// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The audio source on BTstack: A2DP source with BTstack's SBC encoder, an
// AVRCP target that takes the remote's keys and answers its track and play
// status queries, and an AVRCP controller for the sink's own volume. See
// include/btclassic.h.
//
// The program, or any C producer, pushes PCM in with btc_source_write(),
// which resamples to the stream's rate into a ring. A run-loop timer drains
// the ring at the stream's clock, encodes SBC frames and sends them as media
// packets. The writer stops at half full and a little, so the reader's clock
// correction never has to drop. Everything runs on the thread that runs
// BTstack, so nothing here takes a lock.

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"
#include "ring.h"

#define MIN_RING_FRAMES 4096
#define DEFAULT_VOLUME 100
#define KEY_QUEUE 16
#define TEXT_MAX 128
#define TICK_MS 10
#define MAX_CATCH_UP_MS 100     // after a stall, don't send more than this at once
#define SBC_STORAGE 1030        // one media packet: an SBC header byte, then frames

// What the source offers: 44.1 or 48 kHz, any channel mode, any block length,
// subbands and allocation, bitpool 2 to 53. The sink picks within it.
static const uint8_t sbc_caps[] = {
    ((AVDTP_SBC_44100 | AVDTP_SBC_48000) << 4) | AVDTP_SBC_STEREO | AVDTP_SBC_JOINT_STEREO | AVDTP_SBC_MONO,
    0xff,
    2, 53,
};
static uint8_t sbc_config[4];

static struct {
    bool started;
    btc_ring_t ring;
    int16_t *ring_buf;
    uint32_t changes;

    // A2DP
    uint16_t a2dp_cid;
    uint8_t local_seid;
    avdtp_stream_endpoint_t *local_ep;
    bool connected;
    bool stream_open;
    bool streaming;
    bool want_stream;       // start the stream when the link is up
    btc_addr_t peer;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t sbc_channel_mode, sbc_block_length, sbc_subbands, sbc_allocation;
    uint8_t sbc_min_bitpool, sbc_max_bitpool;
    uint16_t delay_ms;
    char text[BTC_SOURCE_TEXT_COUNT][TEXT_MAX];

    // The media clock.
    btstack_timer_source_t timer;
    uint32_t last_ms;
    uint32_t acc;           // frames x 1000 not yet due
    uint32_t frames_due;
    uint32_t rtp_timestamp;
    uint16_t max_payload;
    uint16_t storage_count;
    bool ready_to_send;
    uint8_t storage[SBC_STORAGE];

    // AVRCP
    uint16_t avrcp_cid;
    uint8_t volume;
    bool absolute_volume;   // the sink notifies and takes absolute volume
    uint16_t keys[KEY_QUEUE];
    uint8_t key_head, key_count;

    // What the target tells the remote.
    int track_number, track_count;
    int64_t duration_ms;
    int play_status;
    int64_t pos_ms;
    uint32_t pos_at_ms;
    uint32_t track_id;

    // The writer's resampler.
    uint32_t rs_in_rate, rs_out_rate;
    uint32_t rs_pos;        // Q16: where the next output frame sits, from rs_prev
    int16_t rs_prev[2];
} s;

static btstack_sbc_encoder_state_t encoder;
static btstack_packet_callback_registration_t hci_cb;
static uint8_t sdp_a2dp[150];
static uint8_t sdp_avrcp_tg[200];
static uint8_t sdp_avrcp_ct[200];

static void set_text(btc_source_text_t which, const char *value, size_t len) {
    if (len > TEXT_MAX - 1) {
        len = TEXT_MAX - 1;
        // Don't end partway through a UTF-8 character.
        while (len > 0 && ((uint8_t)value[len] & 0xc0) == 0x80) {
            len--;
        }
    }
    memcpy(s.text[which], value, len);
    s.text[which][len] = 0;
}

// ---- what the target tells the remote ----

static int64_t position_now(void) {
    int64_t pos = s.pos_ms;
    if (pos >= 0 && s.play_status == BTC_PLAY_PLAYING) {
        pos += (uint32_t)(btstack_run_loop_get_time_ms() - s.pos_at_ms);
        if (s.duration_ms > 0 && pos > s.duration_ms) {
            pos = s.duration_ms;
        }
    }
    return pos;
}

static void tell_track(void) {
    if (!s.avrcp_cid) {
        return;
    }
    // BTstack keeps the pointers: the texts live in `s`.
    avrcp_track_t track = {0};
    big_endian_store_32(track.track_id, 4, s.track_id);
    track.track_nr = s.track_number > 0 ? (uint16_t)s.track_number : 0;
    track.title = s.text[BTC_SOURCE_TITLE];
    track.artist = s.text[BTC_SOURCE_ARTIST];
    track.album = s.text[BTC_SOURCE_ALBUM];
    track.genre = s.text[BTC_SOURCE_GENRE];
    track.song_length_ms = s.duration_ms > 0 ? (uint32_t)s.duration_ms : 0xffffffff;
    avrcp_target_set_now_playing_info(s.avrcp_cid, &track, s.track_count > 0 ? (uint16_t)s.track_count : 0);
}

static void tell_play_status(void) {
    if (s.avrcp_cid) {
        avrcp_target_set_playback_status(s.avrcp_cid, (avrcp_playback_status_t)s.play_status);
    }
}

// ---- the media clock: encode what's due and send it ----

static void send_packet(void) {
    uint16_t frame_len = btstack_sbc_encoder_sbc_buffer_length();
    if (!s.a2dp_cid || s.storage_count == 0 || frame_len == 0) {
        s.ready_to_send = false;
        return;
    }
    uint8_t frames = s.storage_count / frame_len;
    s.storage[0] = frames;  // the SBC media header: no fragments, the frame count
    a2dp_source_stream_send_media_payload_rtp(s.a2dp_cid, s.local_seid, 0, s.rtp_timestamp,
        s.storage, s.storage_count + 1);
    s.rtp_timestamp += frames * btstack_sbc_encoder_num_audio_frames();
    s.storage_count = 0;
    s.ready_to_send = false;
}

static void fill(void) {
    uint32_t n = btstack_sbc_encoder_num_audio_frames();
    int32_t gain = s.absolute_volume ? 16129 : (int32_t)s.volume * s.volume;
    int16_t pcm[128 * 2];   // an SBC frame is at most 16 blocks of 8 subbands
    while (s.frames_due >= n && n <= 128) {
        uint16_t frame_len = btstack_sbc_encoder_sbc_buffer_length();
        if (frame_len && s.max_payload - s.storage_count < frame_len) {
            break;
        }
        btc_ring_read(&s.ring, pcm, n, s.channels, gain);
        btstack_sbc_encoder_process_data(pcm);
        frame_len = btstack_sbc_encoder_sbc_buffer_length();
        if (s.storage_count + frame_len > SBC_STORAGE - 1) {
            break;  // only if the payload size changed under us
        }
        memcpy(&s.storage[1 + s.storage_count], btstack_sbc_encoder_sbc_buffer(), frame_len);
        s.storage_count += frame_len;
        s.frames_due -= n;
    }
}

static void tick(btstack_timer_source_t *ts) {
    btstack_run_loop_set_timer(ts, TICK_MS);
    btstack_run_loop_add_timer(ts);
    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t ms = now - s.last_ms;
    s.last_ms = now;
    if (ms > MAX_CATCH_UP_MS) {
        ms = MAX_CATCH_UP_MS;
    }
    s.acc += ms * s.sample_rate;
    s.frames_due += s.acc / 1000;
    s.acc %= 1000;
    if (s.ready_to_send) {
        return;
    }
    fill();
    uint16_t frame_len = btstack_sbc_encoder_sbc_buffer_length();
    if (s.storage_count && s.storage_count + frame_len > s.max_payload) {
        s.ready_to_send = true;
        a2dp_source_stream_endpoint_request_can_send_now(s.a2dp_cid, s.local_seid);
    }
}

static void clock_start(void) {
    int max = a2dp_max_media_payload_size(s.a2dp_cid, s.local_seid);
    s.max_payload = max > 0 && max < SBC_STORAGE - 1 ? (uint16_t)max : SBC_STORAGE - 1;
    s.storage_count = 0;
    s.ready_to_send = false;
    s.acc = 0;
    s.frames_due = 0;
    s.last_ms = btstack_run_loop_get_time_ms();
    btstack_run_loop_remove_timer(&s.timer);
    btstack_run_loop_set_timer_handler(&s.timer, tick);
    btstack_run_loop_set_timer(&s.timer, TICK_MS);
    btstack_run_loop_add_timer(&s.timer);
}

static void clock_stop(void) {
    btstack_run_loop_remove_timer(&s.timer);
    s.storage_count = 0;
    s.ready_to_send = false;
    s.frames_due = 0;
}

// ---- A2DP ----

static void configure(const uint8_t *packet) {
    uint8_t mode = a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet);
    uint8_t alloc = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
    uint8_t blocks = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
    uint8_t subbands = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
    uint8_t bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
    s.sample_rate = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
    s.channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
    s.sbc_channel_mode = btc_sbc_mode_bits(mode);
    s.sbc_block_length = btc_sbc_blocks_bits(blocks);
    s.sbc_subbands = btc_sbc_subbands_bits(subbands);
    s.sbc_allocation = alloc;
    s.sbc_min_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
    s.sbc_max_bitpool = bitpool;
    btstack_sbc_channel_mode_t cm = mode == AVDTP_CHANNEL_MODE_MONO ? SBC_CHANNEL_MODE_MONO
        : mode == AVDTP_CHANNEL_MODE_DUAL_CHANNEL ? SBC_CHANNEL_MODE_DUAL_CHANNEL
        : mode == AVDTP_CHANNEL_MODE_STEREO ? SBC_CHANNEL_MODE_STEREO : SBC_CHANNEL_MODE_JOINT_STEREO;
    // AVDTP counts allocation from 1 (loudness), the encoder from 0.
    btstack_sbc_encoder_init(&encoder, SBC_MODE_STANDARD, blocks, subbands,
        (btstack_sbc_allocation_method_t)(alloc - 1), (int)s.sample_rate, bitpool, cm);
    btc_log("source sbc %u Hz, %u ch, mode %u, blocks %u, subbands %u, bitpool %u", (unsigned)s.sample_rate,
        s.channels, mode, blocks, subbands, bitpool);
    s.changes |= BTC_CHANGED_FORMAT | BTC_CHANGED_PEER;
}

// After stop, the endpoint stays until the link it serves is released:
// freeing it under a live AVDTP connection leaves BTstack pointing into
// the pool.
static avdtp_stream_endpoint_t *stale_ep;

static void release_endpoint(void) {
    if (stale_ep) {
        a2dp_source_finalize_stream_endpoint(stale_ep);
        stale_ep = NULL;
    }
}

static void on_a2dp(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) {
        return;
    }
    if (!s.started) {
        if (hci_event_a2dp_meta_get_subevent_code(packet) == A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED) {
            release_endpoint();
        }
        return;
    }
    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            bd_addr_t addr;
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            uint8_t status = a2dp_subevent_signaling_connection_established_get_status(packet);
            btc_log("source: link to %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                s.a2dp_cid = 0;
                break;
            }
            s.a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            memcpy(s.peer, addr, 6);
            s.connected = true;
            s.text[BTC_SOURCE_PEER_NAME][0] = 0;
            btc_radio_remember_sink(addr);
            gap_remote_name_request(addr, 0, 0);
            s.changes |= BTC_CHANGED_CONNECTION | BTC_CHANGED_PEER;
            break;
        }
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
            configure(packet);
            break;
        case A2DP_SUBEVENT_SIGNALING_DELAY_REPORT:
            s.delay_ms = a2dp_subevent_signaling_delay_report_get_delay_100us(packet) / 10;
            s.changes |= BTC_CHANGED_PEER;
            break;
        case A2DP_SUBEVENT_STREAM_ESTABLISHED: {
            uint8_t status = a2dp_subevent_stream_established_get_status(packet);
            btc_log("source: stream open status 0x%02x", status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            s.a2dp_cid = a2dp_subevent_stream_established_get_a2dp_cid(packet);
            s.stream_open = true;
            if (s.want_stream) {
                a2dp_source_start_stream(s.a2dp_cid, s.local_seid);
            }
            if (!s.avrcp_cid) {
                // For the sink's volume and our track: most sinks call back
                // on their own, and BTstack takes whichever comes first.
                uint16_t cid;
                avrcp_connect(s.peer, &cid);
            }
            break;
        }
        case A2DP_SUBEVENT_STREAM_STARTED:
            s.streaming = true;
            clock_start();
            s.changes |= BTC_CHANGED_STREAM;
            break;
        case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
            send_packet();
            break;
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
            s.streaming = false;
            clock_stop();
            s.changes |= BTC_CHANGED_STREAM;
            break;
        case A2DP_SUBEVENT_STREAM_RELEASED:
            s.streaming = false;
            s.stream_open = false;
            clock_stop();
            s.changes |= BTC_CHANGED_STREAM;
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            btc_log("source: link closed");
            s.connected = false;
            s.streaming = false;
            s.stream_open = false;
            s.a2dp_cid = 0;
            s.delay_ms = 0;
            s.text[BTC_SOURCE_PEER_NAME][0] = 0;
            clock_stop();
            btc_ring_restart(&s.ring);
            s.changes |= BTC_CHANGED_CONNECTION | BTC_CHANGED_STREAM | BTC_CHANGED_PEER;
            break;
        default:
            break;
    }
}

static void on_hci(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE) {
        return;
    }
    bd_addr_t addr;
    hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
    if (s.connected && hci_event_remote_name_request_complete_get_status(packet) == 0 && memcmp(addr, s.peer, 6) == 0) {
        const char *name = hci_event_remote_name_request_complete_get_remote_name(packet);
        set_text(BTC_SOURCE_PEER_NAME, name, strnlen(name, 248));
        s.changes |= BTC_CHANGED_PEER;
    }
}

// ---- AVRCP ----

static void push_key(uint8_t code, bool pressed) {
    if (s.key_count == KEY_QUEUE) {     // full: lose the oldest
        s.key_head = (s.key_head + 1) % KEY_QUEUE;
        s.key_count--;
    }
    s.keys[(s.key_head + s.key_count) % KEY_QUEUE] = code | (pressed ? 0x100 : 0);
    s.key_count++;
    s.changes |= BTC_CHANGED_REMOTE;
}

static void on_avrcp(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
            uint8_t status = avrcp_subevent_connection_established_get_status(packet);
            btc_log("source: avrcp status 0x%02x", status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            s.avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            avrcp_target_support_event(s.avrcp_cid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
            avrcp_target_support_event(s.avrcp_cid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
            avrcp_target_support_event(s.avrcp_cid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
            avrcp_target_support_event(s.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_volume_changed(s.avrcp_cid, s.volume);
            tell_track();
            tell_play_status();
            // The sink's volume, if it takes absolute volume.
            avrcp_controller_enable_notification(s.avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            s.changes |= BTC_CHANGED_PEER;
            break;
        }
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            s.avrcp_cid = 0;
            s.absolute_volume = false;
            s.changes |= BTC_CHANGED_PEER;
            break;
        default:
            break;
    }
}

static void on_avrcp_ct(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    switch (packet[2]) {
        case AVRCP_SUBEVENT_NOTIFICATION_STATE:
            if (avrcp_subevent_notification_state_get_event_id(packet) == AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED) {
                bool on = avrcp_subevent_notification_state_get_status(packet) == ERROR_CODE_SUCCESS
                    && avrcp_subevent_notification_state_get_enabled(packet);
                s.absolute_volume = on;
                if (on) {
                    avrcp_controller_set_absolute_volume(s.avrcp_cid, s.volume);
                }
                s.changes |= BTC_CHANGED_PEER | BTC_CHANGED_VOLUME;
            }
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            s.volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet) & 0x7f;
            s.absolute_volume = true;
            s.changes |= BTC_CHANGED_VOLUME;
            break;
        case AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE:
            s.volume = avrcp_subevent_set_absolute_volume_response_get_absolute_volume(packet) & 0x7f;
            s.changes |= BTC_CHANGED_VOLUME;
            break;
        default:
            break;
    }
}

static void on_avrcp_tg(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    switch (packet[2]) {
        case AVRCP_SUBEVENT_OPERATION:
            push_key(avrcp_subevent_operation_get_operation_id(packet) & 0x7f,
                avrcp_subevent_operation_get_button_pressed(packet) != 0);
            break;
        case AVRCP_SUBEVENT_PLAY_STATUS_QUERY: {
            int64_t pos = position_now();
            avrcp_target_play_status(s.avrcp_cid,
                s.duration_ms > 0 ? (uint32_t)s.duration_ms : 0xffffffff,
                pos >= 0 ? (uint32_t)pos : 0xffffffff,
                (avrcp_playback_status_t)s.play_status);
            break;
        }
        case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
            // The remote set our volume (absolute volume, us as the target).
            s.volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet) & 0x7f;
            s.changes |= BTC_CHANGED_VOLUME;
            break;
        default:
            break;
    }
}

static const btc_av_handlers_t avrcp_handlers = { on_avrcp, on_avrcp_ct, on_avrcp_tg };

// ---- start and stop ----

int btc_source_start(size_t ring_frames) {
    if (s.started || !btc_radio_active() || btc_sink_started()) {
        return BTC_ERR_STATE;
    }
    if (ring_frames < MIN_RING_FRAMES) {
        ring_frames = MIN_RING_FRAMES;
    }
    int16_t *buf = btc_btstack_alloc(ring_frames * 2 * sizeof(int16_t));
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

    btc_av_source_init_once();
    a2dp_source_register_packet_handler(&on_a2dp);
    memcpy(sbc_config, sbc_caps, sizeof(sbc_config));
    // An endpoint still waiting for its old link to go serves again.
    s.local_ep = stale_ep ? stale_ep : a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
        sbc_caps, sizeof(sbc_caps), sbc_config, sizeof(sbc_config));
    stale_ep = NULL;
    if (s.local_ep == NULL) {
        btc_ring_init(&s.ring, NULL, 0, 2);
        btc_btstack_free(buf);
        s.ring_buf = NULL;
        return BTC_ERR_NOMEM;
    }
    s.local_seid = avdtp_local_seid(s.local_ep);
    avdtp_source_register_delay_reporting_category(s.local_seid);
    btc_av_avrcp_use(&avrcp_handlers);

    memset(sdp_a2dp, 0, sizeof(sdp_a2dp));
    a2dp_source_create_sdp_record(sdp_a2dp, sdp_create_service_record_handle(), AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp);
    memset(sdp_avrcp_tg, 0, sizeof(sdp_avrcp_tg));
    avrcp_target_create_sdp_record(sdp_avrcp_tg, sdp_create_service_record_handle(),
        1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
    sdp_register_service(sdp_avrcp_tg);
    memset(sdp_avrcp_ct, 0, sizeof(sdp_avrcp_ct));
    avrcp_controller_create_sdp_record(sdp_avrcp_ct, sdp_create_service_record_handle(),
        1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(sdp_avrcp_ct);

    hci_cb.callback = &on_hci;
    hci_add_event_handler(&hci_cb);
    s.started = true;
    btc_radio_profile(BTC_PROFILE_SOURCE, true, 0);
    return BTC_OK;
}

void btc_source_stop(void) {
    if (!s.started) {
        return;
    }
    btc_radio_profile(BTC_PROFILE_SOURCE, false, 0);
    s.want_stream = false;
    btc_source_disconnect();
    clock_stop();
    sdp_unregister_service(big_endian_read_32(sdp_a2dp, 9));
    sdp_unregister_service(big_endian_read_32(sdp_avrcp_tg, 9));
    sdp_unregister_service(big_endian_read_32(sdp_avrcp_ct, 9));
    hci_remove_event_handler(&hci_cb);
    // The links close on their own. The endpoint goes now if no link uses
    // it, else when its link is released (on_a2dp).
    btc_av_avrcp_use(NULL);
    stale_ep = s.local_ep;
    s.local_ep = NULL;
    bool linked = s.a2dp_cid != 0;
    s.started = false;
    if (!linked) {
        release_endpoint();
    }
    s.connected = s.streaming = s.stream_open = false;
    s.a2dp_cid = s.avrcp_cid = 0;
    btc_ring_init(&s.ring, NULL, 0, 2);
    btc_btstack_free(s.ring_buf);
    s.ring_buf = NULL;
}

void btc_source_radio_stopping(void) {
    btc_source_stop();
}

void btc_source_radio_lost(void) {
    // The heap and BTstack's memory went with the stack.
    stale_ep = NULL;
    memset(&s, 0, sizeof(s));
    btc_ring_init(&s.ring, NULL, 0, 2);
}

bool btc_source_started(void) {
    return s.started;
}

int btc_source_connect(const uint8_t *addr) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    if (s.connected) {
        return BTC_OK;
    }
    bd_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else if (!btc_radio_last_sink(a)) {
        return BTC_ERR_ARG;
    }
    s.want_stream = true;
    memcpy(s.peer, a, 6);
    uint16_t cid;
    uint8_t status = a2dp_source_establish_stream(a, &cid);
    btc_log("source: calling %s: 0x%02x", bd_addr_to_str(a), status);
    if (status != ERROR_CODE_SUCCESS) {
        return BTC_ERR_FAIL;
    }
    s.a2dp_cid = cid;
    return BTC_OK;
}

int btc_source_disconnect(void) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    if (s.avrcp_cid) {
        avrcp_disconnect(s.avrcp_cid);
    }
    if (s.a2dp_cid) {
        a2dp_source_disconnect(s.a2dp_cid);
    }
    return BTC_OK;
}

int btc_source_stream(bool on) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    s.want_stream = on;
    if (!s.connected || !s.stream_open) {
        return BTC_OK;     // it starts when the stream opens
    }
    uint8_t status = ERROR_CODE_SUCCESS;
    if (on && !s.streaming) {
        status = a2dp_source_start_stream(s.a2dp_cid, s.local_seid);
    } else if (!on && s.streaming) {
        status = a2dp_source_pause_stream(s.a2dp_cid, s.local_seid);
    }
    return status == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

// ---- writing ----

// How many frames the writer may add now: up to half the ring and an
// eighth of that, the reader's target plus half its slack.
static uint32_t write_room(void) {
    btc_ring_t *r = &s.ring;
    if (r->buf == NULL) {
        return 0;
    }
    uint32_t used = r->write_pos - r->read_pos;
    if (r->reset) {
        used = 0;   // the reader starts over from what's written next
    }
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

uint32_t btc_source_changes(void) {
    uint32_t c = s.changes;
    s.changes = 0;
    return c;
}

void btc_source_status(btc_source_status_t *out) {
    out->connected = s.connected;
    out->streaming = s.streaming;
    memcpy(out->peer, s.peer, sizeof(btc_addr_t));
    out->sample_rate = s.connected ? s.sample_rate : 0;
    out->channels = s.channels;
    out->volume = s.volume;
    out->absolute_volume = s.absolute_volume;
    out->avrcp = s.avrcp_cid != 0;
    out->sbc_channel_mode = s.sbc_channel_mode;
    out->sbc_block_length = s.sbc_block_length;
    out->sbc_subbands = s.sbc_subbands;
    out->sbc_allocation = s.sbc_allocation;
    out->sbc_min_bitpool = s.sbc_min_bitpool;
    out->sbc_max_bitpool = s.sbc_max_bitpool;
    out->delay_ms = s.delay_ms;
    out->metadata = true;
    out->play_status = s.play_status;
    out->position_ms = position_now();
}

void btc_source_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&s.ring, out);
}

size_t btc_source_text(btc_source_text_t which, char *dst, size_t size) {
    if (which >= BTC_SOURCE_TEXT_COUNT || size == 0) {
        return 0;
    }
    size_t n = strlen(s.text[which]);
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(dst, s.text[which], n);
    dst[n] = 0;
    return n;
}

void btc_source_set_volume(uint8_t volume) {
    s.volume = volume & 0x7f;
    if (s.avrcp_cid) {
        if (s.absolute_volume) {
            avrcp_controller_set_absolute_volume(s.avrcp_cid, s.volume);
        }
        // A remote that registered for our volume hears of it.
        avrcp_target_volume_changed(s.avrcp_cid, s.volume);
    }
    s.changes |= BTC_CHANGED_VOLUME;
}

int btc_source_key(void) {
    if (!s.key_count) {
        return -1;
    }
    int key = s.keys[s.key_head];
    s.key_head = (s.key_head + 1) % KEY_QUEUE;
    s.key_count--;
    return key;
}

static bool set_field(btc_source_text_t which, const char *text) {
    if (text == NULL) {
        return false;
    }
    bool changed = strncmp(s.text[which], text, TEXT_MAX - 1) != 0;
    set_text(which, text, strlen(text));
    return changed;
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
    s.track_number = track_number;
    s.track_count = track_count;
    s.duration_ms = duration_ms;
    if (new_track) {
        s.track_id++;
        s.pos_ms = 0;
        s.pos_at_ms = btstack_run_loop_get_time_ms();
    }
    tell_track();
    return BTC_OK;
}

int btc_source_set_play_status(int status, int64_t position_ms) {
    if (!s.started) {
        return BTC_ERR_STATE;
    }
    if (status < 0) {
        status = s.play_status;
    }
    s.pos_ms = position_ms >= 0 ? position_ms : position_now();
    s.pos_at_ms = btstack_run_loop_get_time_ms();
    bool changed = s.play_status != status;
    s.play_status = status;
    if (changed) {
        tell_play_status();
    }
    return BTC_OK;
}
