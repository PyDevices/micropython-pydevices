// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's audio sink on BTstack: A2DP sink (SBC, decoded here), the
// AVRCP controller (metadata, play status, position, keys, player settings,
// cover art over BIP) and the AVRCP target (absolute volume).

#include <string.h>

#include "btstack.h"
#include "classic/avrcp_cover_art_client.h"
#include "classic/btstack_sbc.h"

#include "btc_btstack.h"
#include "ring.h"

#ifndef BTC_SINK_MAX_RING_FRAMES
#define BTC_SINK_MAX_RING_FRAMES 24000      // 94 KB: half a second at 48 kHz
#endif
#ifndef BTC_SINK_COVER_MAX
#define BTC_SINK_COVER_MAX (48 * 1024)
#endif
#define TEXT_MAX 128

static bool started;
static uint32_t changed;
static btc_sink_status_t st;
static char text[BTC_SINK_TEXT_COUNT][TEXT_MAX];

static int16_t *ring_buf;
static btc_ring_t ring;

// A2DP
static uint8_t sdp_a2dp[150];
static uint8_t sdp_avrcp_ct[200];
static uint8_t sdp_avrcp_tg[150];
static uint8_t sbc_caps[] = { 0xff, 0xff, 2, 53 };
static uint8_t sbc_config[4];
static uint8_t local_seid;
static uint16_t a2dp_cid;
static hci_con_handle_t a2dp_con;
static btstack_sbc_decoder_state_t sbc;
static bool sbc_ready;

// AVRCP
static uint16_t avrcp_cid;
static uint32_t notifications;      // the events the sender supports
static btstack_timer_source_t poll_timer;
static uint32_t position_at_ms;     // when position_ms was reported

// Cover art
static avrcp_cover_art_client_t cover_client;
static uint16_t cover_cid;
static bool cover_connected;
static char image_handle[8];
static char cover_fetching[8];
static uint8_t cover_ertm_buf[2000];
static l2cap_ertm_config_t cover_ertm = { 1, 2, 2000, 12000, 512, 2, 2, 1 };
static uint8_t *cover;             // BTC_SINK_COVER_MAX, while the cover art channel is open
static size_t cover_len;            // the finished image
static size_t cover_rx;             // bytes of the one arriving
static bool cover_overflow;
static uint32_t cover_serial;

static btstack_packet_callback_registration_t hci_cb;

static void set_text(btc_sink_text_t which, const uint8_t *value, size_t len) {
    if (len > TEXT_MAX - 1) {
        len = TEXT_MAX - 1;
    }
    if (strlen(text[which]) == len && memcmp(text[which], value, len) == 0) {
        return;
    }
    memcpy(text[which], value, len);
    text[which][len] = 0;
    changed |= which == BTC_SINK_PEER_NAME ? BTC_CHANGED_PEER : BTC_CHANGED_TRACK;
}

static void clear_track(void) {
    for (int i = BTC_SINK_TITLE; i <= BTC_SINK_GENRE; i++) {
        text[i][0] = 0;
    }
    st.track_number = BTC_UNKNOWN;
    st.track_count = BTC_UNKNOWN;
    st.duration_ms = BTC_UNKNOWN;
    st.position_ms = BTC_UNKNOWN;
    image_handle[0] = 0;
    changed |= BTC_CHANGED_TRACK;
}

static void clear_cover(void) {
    if (cover_len) {
        cover_len = 0;
        cover_serial++;
        changed |= BTC_CHANGED_COVER;
    }
}

// ---- decoding ----

static void on_pcm(int16_t *data, int frames, int channels, int sample_rate, void *context) {
    (void)context;
    if ((uint32_t)sample_rate != st.sample_rate) {
        st.sample_rate = sample_rate;
        changed |= BTC_CHANGED_FORMAT;
    }
    btc_ring_write(&ring, data, frames, channels);
}

static void on_media(uint8_t seid, uint8_t *packet, uint16_t size) {
    (void)seid;
    if (!sbc_ready) {
        return;
    }
    // RTP header (12 bytes plus CSRCs), then the SBC media payload header.
    if (size < 13) {
        return;
    }
    int pos = 12 + 4 * (packet[0] & 0x0f);
    if (pos >= size) {
        return;
    }
    pos++;      // SBC payload header: fragmentation and frame count
    if (!st.streaming) {
        st.streaming = true;
        changed |= BTC_CHANGED_STREAM;
    }
    btstack_sbc_decoder_process_data(&sbc, 0, packet + pos, size - pos);
    static uint32_t packets;
    if ((packets++ % 2000) == 0) {
        const uint8_t *f = packet + pos;
        btc_log("media %u bytes, frames %u: %02x %02x %02x %02x %02x %02x %02x %02x; good %u bad %u zero %u; peak %u",
            size, packet[pos - 1] & 0x0f, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
            sbc.good_frames_nr, sbc.bad_frames_nr, sbc.zero_frames_nr, (unsigned)ring.peak_l);
    }
}

// ---- AVRCP ----

static void poll_status(btstack_timer_source_t *ts) {
    if (avrcp_cid) {
        avrcp_controller_get_play_status(avrcp_cid);
        if (a2dp_con != HCI_CON_HANDLE_INVALID) {
            gap_read_rssi(a2dp_con);
        }
        btstack_run_loop_set_timer(ts, 1000);
        btstack_run_loop_add_timer(ts);
    }
}

static void cover_fetch(void) {
    if (!cover_connected || !image_handle[0] || strcmp(image_handle, cover_fetching) == 0) {
        return;
    }
    if (cover_fetching[0] && cover_rx) {
        return;     // one at a time; the next track's comes after
    }
    memcpy(cover_fetching, image_handle, sizeof(cover_fetching));
    cover_rx = 0;
    cover_overflow = false;
    if (avrcp_cover_art_client_get_linked_thumbnail(cover_cid, cover_fetching) != ERROR_CODE_SUCCESS) {
        cover_fetching[0] = 0;
    }
}

static void on_cover(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == BIP_DATA_PACKET) {
        if (cover && cover_rx + size <= BTC_SINK_COVER_MAX) {
            // Assemble past the finished image's end only when it's gone:
            // the new one overwrites from 0 once it is complete.
            memcpy(cover + cover_rx, packet, size);
        } else {
            cover_overflow = true;
        }
        if (cover_len) {
            cover_len = 0;  // the old image is being overwritten
            cover_serial++;
        }
        cover_rx += size;
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    switch (hci_event_avrcp_meta_get_subevent_code(packet)) {
        case AVRCP_SUBEVENT_COVER_ART_CONNECTION_ESTABLISHED:
            if (!started) {
                break;  // came up as the sink stopped: it closes with the link
            }
            if (avrcp_subevent_cover_art_connection_established_get_status(packet) == ERROR_CODE_SUCCESS) {
                if (cover == NULL) {
                    cover = btc_btstack_alloc(BTC_SINK_COVER_MAX);
                }
                cover_connected = true;
                st.cover_art = true;
                btc_log("cover art channel open");
                cover_fetch();
            } else {
                btc_log("cover art connect failed 0x%02x",
                    avrcp_subevent_cover_art_connection_established_get_status(packet));
                cover_cid = 0;
            }
            changed |= BTC_CHANGED_PEER;
            break;
        case AVRCP_SUBEVENT_COVER_ART_OPERATION_COMPLETE:
            if (cover_fetching[0]) {
                bool ok = !cover_overflow && cover_rx > 0;
                btc_log("cover %s: %u bytes%s", cover_fetching, (unsigned)cover_rx, ok ? "" : " (dropped)");
                cover_len = ok ? cover_rx : 0;
                cover_serial++;
                cover_rx = 0;
                changed |= BTC_CHANGED_COVER;
                if (strcmp(cover_fetching, image_handle) != 0) {
                    cover_fetching[0] = 0;
                    cover_fetch();
                }
            }
            break;
        case AVRCP_SUBEVENT_COVER_ART_CONNECTION_RELEASED:
            cover_connected = false;
            cover_cid = 0;
            cover_fetching[0] = 0;
            st.cover_art = false;
            clear_cover();
            btc_btstack_free(cover);
            cover = NULL;
            changed |= BTC_CHANGED_PEER;
            break;
        default:
            break;
    }
}

static void on_avrcp(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    bd_addr_t addr;
    switch (packet[2]) {
        case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
            uint8_t status = avrcp_subevent_connection_established_get_status(packet);
            avrcp_subevent_connection_established_get_bd_addr(packet, addr);
            btc_log("avrcp %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                avrcp_cid = 0;
                return;
            }
            avrcp_cid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
            notifications = 0;
            avrcp_target_support_event(avrcp_cid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
            avrcp_target_support_event(avrcp_cid, AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED);
            avrcp_controller_get_supported_events(avrcp_cid);
            btstack_run_loop_set_timer_handler(&poll_timer, poll_status);
            btstack_run_loop_set_timer(&poll_timer, 1000);
            btstack_run_loop_add_timer(&poll_timer);
            changed |= BTC_CHANGED_CONNECTION;
            break;
        }
        case AVRCP_SUBEVENT_CONNECTION_RELEASED:
            btc_log("avrcp released");
            avrcp_cid = 0;
            btstack_run_loop_remove_timer(&poll_timer);
            if (cover_cid) {
                avrcp_cover_art_client_disconnect(cover_cid);
            }
            changed |= BTC_CHANGED_CONNECTION;
            break;
        default:
            break;
    }
}

static void on_avrcp_ct(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META || !avrcp_cid) {
        return;
    }
    switch (packet[2]) {
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID:
            notifications |= 1u << avrcp_subevent_get_capability_event_id_get_event_id(packet);
            break;
        case AVRCP_SUBEVENT_GET_CAPABILITY_EVENT_ID_DONE: {
            static const uint8_t want[] = {
                AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED,
                AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED,
                AVRCP_NOTIFICATION_EVENT_PLAYBACK_POS_CHANGED,
                AVRCP_NOTIFICATION_EVENT_BATT_STATUS_CHANGED,
                AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED,
                AVRCP_NOTIFICATION_EVENT_PLAYER_APPLICATION_SETTING_CHANGED,
                AVRCP_NOTIFICATION_EVENT_UIDS_CHANGED,
            };
            st.avrcp_features = notifications;
            for (size_t i = 0; i < sizeof(want); i++) {
                if (notifications & (1u << want[i])) {
                    avrcp_controller_enable_notification(avrcp_cid, (avrcp_notification_event_id_t)want[i]);
                }
            }
            avrcp_controller_get_now_playing_info(avrcp_cid);
            // Cover art: BIP over L2CAP, if the sender has it.
            if (!cover_cid) {
                bd_addr_t peer;
                memcpy(peer, st.peer, 6);
                uint8_t s = avrcp_cover_art_client_connect(&cover_client, on_cover, peer,
                    cover_ertm_buf, sizeof(cover_ertm_buf), &cover_ertm, &cover_cid);
                if (s != ERROR_CODE_SUCCESS) {
                    btc_log("cover art connect: 0x%02x", s);
                    cover_cid = 0;
                }
            }
            break;
        }
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_STATUS_CHANGED: {
            int ps = avrcp_subevent_notification_playback_status_changed_get_play_status(packet);
            if (ps != st.play_status) {
                st.play_status = ps;
                changed |= BTC_CHANGED_PLAYING;
            }
            break;
        }
        case AVRCP_SUBEVENT_NOTIFICATION_TRACK_CHANGED:
        case AVRCP_SUBEVENT_NOTIFICATION_NOW_PLAYING_CONTENT_CHANGED:
            avrcp_controller_get_now_playing_info(avrcp_cid);
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_UIDS_CHANGED:
            // Image handles go stale with a new player.
            image_handle[0] = 0;
            cover_fetching[0] = 0;
            break;
        case AVRCP_SUBEVENT_NOTIFICATION_PLAYBACK_POS_CHANGED: {
            uint32_t p = avrcp_subevent_notification_playback_pos_changed_get_playback_position_ms(packet);
            st.position_ms = p == 0xffffffffu ? BTC_UNKNOWN : (int64_t)p;
            position_at_ms = btstack_run_loop_get_time_ms();
            changed |= BTC_CHANGED_POSITION;
            break;
        }
        case AVRCP_SUBEVENT_NOTIFICATION_EVENT_BATT_STATUS_CHANGED:
            st.battery = avrcp_subevent_notification_event_batt_status_changed_get_battery_status(packet);
            changed |= BTC_CHANGED_PEER;
            break;
        case AVRCP_SUBEVENT_PLAY_STATUS: {
            uint32_t len = avrcp_subevent_play_status_get_song_length(packet);
            uint32_t p = avrcp_subevent_play_status_get_song_position(packet);
            int ps = avrcp_subevent_play_status_get_play_status(packet);
            if (len != 0xffffffffu && (int64_t)len != st.duration_ms) {
                st.duration_ms = len;
                changed |= BTC_CHANGED_TRACK;
            }
            st.position_ms = p == 0xffffffffu ? BTC_UNKNOWN : (int64_t)p;
            position_at_ms = btstack_run_loop_get_time_ms();
            changed |= BTC_CHANGED_POSITION;
            if (ps != st.play_status) {
                st.play_status = ps;
                changed |= BTC_CHANGED_PLAYING;
            }
            break;
        }
        case AVRCP_SUBEVENT_NOW_PLAYING_TRACK_INFO:
            st.track_number = avrcp_subevent_now_playing_track_info_get_track(packet);
            changed |= BTC_CHANGED_TRACK;
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TOTAL_TRACKS_INFO:
            st.track_count = avrcp_subevent_now_playing_total_tracks_info_get_total_tracks(packet);
            changed |= BTC_CHANGED_TRACK;
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_SONG_LENGTH_MS_INFO:
            st.duration_ms = avrcp_subevent_now_playing_song_length_ms_info_get_song_length(packet);
            changed |= BTC_CHANGED_TRACK;
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_TITLE_INFO:
            set_text(BTC_SINK_TITLE, avrcp_subevent_now_playing_title_info_get_value(packet),
                avrcp_subevent_now_playing_title_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ARTIST_INFO:
            set_text(BTC_SINK_ARTIST, avrcp_subevent_now_playing_artist_info_get_value(packet),
                avrcp_subevent_now_playing_artist_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_ALBUM_INFO:
            set_text(BTC_SINK_ALBUM, avrcp_subevent_now_playing_album_info_get_value(packet),
                avrcp_subevent_now_playing_album_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_GENRE_INFO:
            set_text(BTC_SINK_GENRE, avrcp_subevent_now_playing_genre_info_get_value(packet),
                avrcp_subevent_now_playing_genre_info_get_value_len(packet));
            break;
        case AVRCP_SUBEVENT_NOW_PLAYING_COVER_ART_INFO: {
            uint16_t n = avrcp_subevent_now_playing_cover_art_info_get_value_len(packet);
            if (n == 7) {
                char h[8];
                memcpy(h, avrcp_subevent_now_playing_cover_art_info_get_value(packet), 7);
                h[7] = 0;
                if (strcmp(h, image_handle) != 0) {
                    memcpy(image_handle, h, 8);
                    cover_fetch();
                }
            }
            break;
        }
        case AVRCP_SUBEVENT_NOW_PLAYING_INFO_DONE:
            if (!image_handle[0]) {
                clear_cover();
            }
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
    if (packet[2] == AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED) {
        st.volume = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet) & 0x7f;
        changed |= BTC_CHANGED_VOLUME;
    }
}

// ---- A2DP ----

static avdtp_stream_endpoint_t *local_ep;

// After stop, the endpoint stays until the link it serves is released:
// freeing it under a live AVDTP connection leaves BTstack pointing into
// the pool.
static void release_endpoint(void) {
    if (local_ep) {
        a2dp_sink_finalize_stream_endpoint(local_ep);
        local_ep = NULL;
    }
}

static void on_a2dp(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) {
        return;
    }
    if (!started) {
        if (packet[2] == A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED) {
            a2dp_cid = 0;
            release_endpoint();
        }
        return;
    }
    bd_addr_t addr;
    switch (packet[2]) {
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED: {
            uint8_t status = a2dp_subevent_signaling_connection_established_get_status(packet);
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            btc_log("a2dp signaling %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            a2dp_cid = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
            a2dp_con = a2dp_subevent_signaling_connection_established_get_con_handle(packet);
            memcpy(st.peer, addr, 6);
            st.connected = true;
            btc_radio_remember_peer(addr);
            gap_remote_name_request(addr, 0, 0);
            // The hands-free unit follows the sender, as a car kit does.
            if (btc_hf_started()) {
                btc_hf_status_t hf;
                btc_hf_status(&hf);
                if (!hf.connected) {
                    btc_hf_connect(addr);
                }
            }
            changed |= BTC_CHANGED_CONNECTION;
            break;
        }
        case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION: {
            st.channels = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            st.sbc_channel_mode = btc_sbc_mode_bits(a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet));
            st.sbc_block_length = btc_sbc_blocks_bits(a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet));
            st.sbc_subbands = btc_sbc_subbands_bits(a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet));
            st.sbc_allocation = a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet);
            st.sbc_min_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            st.sbc_max_bitpool = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            uint32_t rate = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            btc_log("sbc %u Hz, %u ch, mode %u, blocks %u, subbands %u, bitpool %u..%u", (unsigned)rate,
                st.channels, st.sbc_channel_mode, st.sbc_block_length, st.sbc_subbands,
                st.sbc_min_bitpool, st.sbc_max_bitpool);
            if (rate != st.sample_rate) {
                st.sample_rate = rate;
                changed |= BTC_CHANGED_FORMAT;
            }
            changed |= BTC_CHANGED_PEER;
            break;
        }
        case A2DP_SUBEVENT_STREAM_ESTABLISHED:
            btc_log("a2dp stream status 0x%02x", a2dp_subevent_stream_established_get_status(packet));
            break;
        case A2DP_SUBEVENT_STREAM_STARTED:
            btc_log("a2dp stream started");
            btstack_sbc_decoder_init(&sbc, SBC_MODE_STANDARD, on_pcm, NULL);
            sbc_ready = true;
            btc_ring_restart(&ring);
            break;
        case A2DP_SUBEVENT_STREAM_SUSPENDED:
        case A2DP_SUBEVENT_STREAM_STOPPED:
        case A2DP_SUBEVENT_STREAM_RELEASED:
            btc_log("a2dp stream stopped (%u)", packet[2]);
            sbc_ready = false;
            if (st.streaming) {
                st.streaming = false;
                changed |= BTC_CHANGED_STREAM;
            }
            break;
        case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED:
            btc_log("a2dp released");
            a2dp_cid = 0;
            a2dp_con = HCI_CON_HANDLE_INVALID;
            sbc_ready = false;
            st.connected = false;
            st.streaming = false;
            clear_track();
            clear_cover();
            changed |= BTC_CHANGED_CONNECTION | BTC_CHANGED_STREAM;
            break;
        default:
            break;
    }
}

static void on_hci(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE: {
            bd_addr_t addr;
            hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
            if (hci_event_remote_name_request_complete_get_status(packet) == 0 && memcmp(addr, st.peer, 6) == 0) {
                const char *name = hci_event_remote_name_request_complete_get_remote_name(packet);
                set_text(BTC_SINK_PEER_NAME, (const uint8_t *)name, strnlen(name, 248));
            }
            break;
        }
        case GAP_EVENT_RSSI_MEASUREMENT:
            if (gap_event_rssi_measurement_get_con_handle(packet) == a2dp_con) {
                int r = (int8_t)gap_event_rssi_measurement_get_rssi(packet);
                if (r != st.rssi_delta) {
                    st.rssi_delta = r;
                    changed |= BTC_CHANGED_PEER;
                }
            }
            break;
        default:
            break;
    }
}

static const btc_av_handlers_t avrcp_handlers = { on_avrcp, on_avrcp_ct, on_avrcp_tg };

// ---- the API ----

int btc_sink_start(size_t ring_frames, bool reconnect) {
    if (started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (ring_frames == 0 || ring_frames > BTC_SINK_MAX_RING_FRAMES) {
        ring_frames = BTC_SINK_MAX_RING_FRAMES;
    }
    memset(&st, 0, sizeof(st));
    memset(text, 0, sizeof(text));
    st.volume = 100;
    st.play_status = BTC_UNKNOWN;
    st.battery = BTC_UNKNOWN;
    clear_track();
    changed = 0;
    a2dp_con = HCI_CON_HANDLE_INVALID;
    ring_buf = btc_btstack_alloc(ring_frames * 2 * sizeof(int16_t));
    if (ring_buf == NULL) {
        return BTC_ERR_NOMEM;
    }
    btc_ring_init(&ring, ring_buf, ring_frames, 2);

    btc_av_sink_init_once();
    btc_av_avrcp_use(&avrcp_handlers);
    a2dp_sink_register_packet_handler(&on_a2dp);
    a2dp_sink_register_media_handler(&on_media);
    // An endpoint still waiting for its old link to go serves again.
    avdtp_stream_endpoint_t *ep = local_ep ? local_ep : a2dp_sink_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
        sbc_caps, sizeof(sbc_caps), sbc_config, sizeof(sbc_config));
    if (ep == NULL) {
        btc_av_avrcp_use(NULL);
        btc_ring_init(&ring, NULL, 0, 2);
        btc_btstack_free(ring_buf);
        ring_buf = NULL;
        return BTC_ERR_NOMEM;
    }
    local_ep = ep;
    local_seid = avdtp_local_seid(ep);

    memset(sdp_a2dp, 0, sizeof(sdp_a2dp));
    a2dp_sink_create_sdp_record(sdp_a2dp, sdp_create_service_record_handle(), AVDTP_SINK_FEATURE_MASK_SPEAKER, NULL, NULL);
    sdp_register_service(sdp_a2dp);
    memset(sdp_avrcp_ct, 0, sizeof(sdp_avrcp_ct));
    uint16_t ct_features = (1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_CATEGORY_PLAYER_OR_RECORDER)
        | (1 << AVRCP_CONTROLLER_SUPPORTED_FEATURE_COVER_ART_GET_LINKED_THUMBNAIL);
    avrcp_controller_create_sdp_record(sdp_avrcp_ct, sdp_create_service_record_handle(), ct_features, NULL, NULL);
    sdp_register_service(sdp_avrcp_ct);
    memset(sdp_avrcp_tg, 0, sizeof(sdp_avrcp_tg));
    avrcp_target_create_sdp_record(sdp_avrcp_tg, sdp_create_service_record_handle(),
        1 << AVRCP_TARGET_SUPPORTED_FEATURE_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(sdp_avrcp_tg);

    hci_cb.callback = &on_hci;
    hci_add_event_handler(&hci_cb);

    started = true;
    btc_radio_profile(BTC_PROFILE_SINK, true, 0);
    if (reconnect) {
        btc_sink_connect(NULL);
    }
    return BTC_OK;
}

void btc_sink_stop(void) {
    if (!started) {
        return;
    }
    btc_radio_profile(BTC_PROFILE_SINK, false, 0);
    btc_sink_disconnect();
    btstack_run_loop_remove_timer(&poll_timer);
    sdp_unregister_service(big_endian_read_32(sdp_a2dp, 9));
    sdp_unregister_service(big_endian_read_32(sdp_avrcp_ct, 9));
    sdp_unregister_service(big_endian_read_32(sdp_avrcp_tg, 9));
    hci_remove_event_handler(&hci_cb);
    // The links close on their own. The endpoint goes now if no link uses
    // it, else when its link is released (on_a2dp), so the next start, or
    // the source, can have it.
    btc_av_avrcp_use(NULL);
    started = false;
    if (!a2dp_cid) {
        release_endpoint();
    }
    sbc_ready = false;
    avrcp_cid = 0;
    a2dp_cid = 0;
    cover_cid = 0;
    cover_len = 0;
    btc_ring_init(&ring, NULL, 0, 2);
    btc_btstack_free(ring_buf);
    ring_buf = NULL;
    btc_btstack_free(cover);
    cover = NULL;
}

void btc_sink_radio_stopping(void) {
    btc_sink_stop();
}

bool btc_sink_started(void) {
    return started;
}

size_t btc_sink_read(int16_t *dst, size_t frames) {
    int32_t level = st.volume;
    return btc_ring_read(&ring, dst, frames, 2, level * level);     // about a dB taper
}

uint32_t btc_sink_changes(void) {
    uint32_t c = changed;
    changed = 0;
    return c;
}

void btc_sink_status(btc_sink_status_t *out) {
    *out = st;
    if (st.play_status == BTC_PLAY_PLAYING && st.position_ms >= 0) {
        out->position_ms += btstack_run_loop_get_time_ms() - position_at_ms;
        if (st.duration_ms > 0 && out->position_ms > st.duration_ms) {
            out->position_ms = st.duration_ms;
        }
    }
}

void btc_sink_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&ring, out);
}

size_t btc_sink_text(btc_sink_text_t which, char *dst, size_t size) {
    if ((unsigned)which >= BTC_SINK_TEXT_COUNT || size == 0) {
        return 0;
    }
    size_t n = strlen(text[which]);
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(dst, text[which], n);
    dst[n] = 0;
    return n;
}

size_t btc_sink_cover_size(void) {
    return cover_len;
}

size_t btc_sink_cover(uint8_t *dst, size_t size) {
    if (!cover_len || !cover || size < cover_len) {
        return 0;
    }
    memcpy(dst, cover, cover_len);
    return cover_len;
}

void btc_sink_levels(uint16_t *left, uint16_t *right) {
    btc_ring_levels(&ring, left, right);
}

int btc_sink_key(uint8_t key, int state) {
    if (!avrcp_cid) {
        return BTC_ERR_STATE;
    }
    if (key > 0x7f || state < 0 || state > 2) {
        return BTC_ERR_ARG;
    }
    uint8_t s;
    if (state == 0) {
        s = avrcp_controller_start_press_and_hold_cmd(avrcp_cid, (avrcp_operation_id_t)key);
    } else if (state == 1) {
        s = avrcp_controller_release_press_and_hold_cmd(avrcp_cid);
    } else {
        // A press and its release: BTstack sends the release itself.
        switch (key) {
            case AVRCP_OPERATION_ID_PLAY: s = avrcp_controller_play(avrcp_cid); break;
            case AVRCP_OPERATION_ID_STOP: s = avrcp_controller_stop(avrcp_cid); break;
            case AVRCP_OPERATION_ID_PAUSE: s = avrcp_controller_pause(avrcp_cid); break;
            case AVRCP_OPERATION_ID_REWIND: s = avrcp_controller_rewind(avrcp_cid); break;
            case AVRCP_OPERATION_ID_FAST_FORWARD: s = avrcp_controller_fast_forward(avrcp_cid); break;
            case AVRCP_OPERATION_ID_FORWARD: s = avrcp_controller_forward(avrcp_cid); break;
            case AVRCP_OPERATION_ID_BACKWARD: s = avrcp_controller_backward(avrcp_cid); break;
            case AVRCP_OPERATION_ID_VOLUME_UP: s = avrcp_controller_volume_up(avrcp_cid); break;
            case AVRCP_OPERATION_ID_VOLUME_DOWN: s = avrcp_controller_volume_down(avrcp_cid); break;
            case AVRCP_OPERATION_ID_MUTE: s = avrcp_controller_mute(avrcp_cid); break;
            default: s = avrcp_controller_force_send_press_cmd(avrcp_cid, (avrcp_operation_id_t)key); break;
        }
    }
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

int btc_sink_set_player(uint8_t attr, uint8_t value) {
    if (!avrcp_cid) {
        return BTC_ERR_STATE;
    }
    uint8_t s;
    switch (attr) {
        case 2: s = avrcp_controller_set_repeat_mode(avrcp_cid, (avrcp_repeat_mode_t)value); break;
        case 3: s = avrcp_controller_set_shuffle_mode(avrcp_cid, (avrcp_shuffle_mode_t)value); break;
        default: return BTC_ERR_UNSUPPORTED;     // equalizer and scan: BTstack has no call for them
    }
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

int btc_sink_refresh(void) {
    if (!avrcp_cid) {
        return BTC_ERR_STATE;
    }
    avrcp_controller_get_play_status(avrcp_cid);
    if (a2dp_con != HCI_CON_HANDLE_INVALID) {
        gap_read_rssi(a2dp_con);
    }
    return BTC_OK;
}

void btc_sink_set_volume(uint8_t volume) {
    st.volume = volume & 0x7f;
    if (avrcp_cid) {
        avrcp_target_volume_changed(avrcp_cid, st.volume);
    }
    changed |= BTC_CHANGED_VOLUME;
}

int btc_sink_connect(const uint8_t *addr) {
    if (!started) {
        return BTC_ERR_STATE;
    }
    bd_addr_t peer;
    if (addr) {
        memcpy(peer, addr, 6);
    } else if (!btc_radio_last_peer(peer)) {
        return BTC_ERR_STATE;
    }
    uint16_t cid;
    uint8_t s = a2dp_sink_establish_stream(peer, &cid);
    btc_log("calling %s: 0x%02x", bd_addr_to_str(peer), s);
    if (s != ERROR_CODE_SUCCESS) {
        return BTC_ERR_FAIL;
    }
    avrcp_connect(peer, &cid);
    return BTC_OK;
}

int btc_sink_disconnect(void) {
    if (!a2dp_cid && !avrcp_cid) {
        return BTC_ERR_STATE;
    }
    if (avrcp_cid) {
        avrcp_disconnect(avrcp_cid);
    }
    if (a2dp_cid) {
        a2dp_sink_disconnect(a2dp_cid);
    }
    return BTC_OK;
}

void btc_sink_radio_lost(void) {
    started = false;
    local_ep = NULL;
    sbc_ready = false;
    avrcp_cid = 0;
    a2dp_cid = 0;
    cover_cid = 0;
    cover_connected = false;
    memset(&st, 0, sizeof(st));
    // The heap went with the stack.
    btc_ring_init(&ring, NULL, 0, 2);
    ring_buf = NULL;
    cover = NULL;
    cover_len = 0;
}
