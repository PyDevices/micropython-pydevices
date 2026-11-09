// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's hands-free unit on BTstack: HFP HF over RFCOMM, the phone's
// indicators and calls, and the call's audio over SCO through HCI: CVSD at
// 8 kHz or mSBC at 16 kHz, coded here.

#include <string.h>

#include "btstack.h"
#include "classic/btstack_cvsd_plc.h"
#include "classic/btstack_sbc.h"
#include "classic/hfp_codec.h"

#include "btc_btstack.h"
#include "ring.h"

#define HF_RFCOMM_CHANNEL 1
#define TEXT_MAX 64
#define MAX_RING_FRAMES 8000        // 500 ms at 16 kHz, mono
#define MIC_FRAMES 8000             // half a second at 16 kHz
#define MIC_PREBUFFER_MS 120

static bool started;
static uint32_t changed;
static btc_hf_status_t st;
static char text[BTC_HF_TEXT_COUNT][TEXT_MAX];
static uint8_t sdp_hf[150];
static hci_con_handle_t acl = HCI_CON_HANDLE_INVALID;
static hci_con_handle_t sco = HCI_CON_HANDLE_INVALID;
static uint8_t codec;
static int callsetup, call_on;
static btstack_packet_callback_registration_t hci_cb;

// The phone's voice.
static int16_t *ring_buf;           // MAX_RING_FRAMES, while started
static btc_ring_t ring;
static uint32_t ring_ms = 200;
static uint8_t out_channels = 1;
static btstack_cvsd_plc_state_t cvsd_plc;
static btstack_sbc_decoder_state_t msbc_dec;

// The microphone, to the phone.
static int16_t *mic;                // MIC_FRAMES, while started
static uint32_t mic_head, mic_count;
static bool mic_paused = true;
static btstack_sbc_encoder_state_t msbc_enc;
static hfp_codec_t hfp_codec;

static void set_text(btc_hf_text_t which, const char *value) {
    if (strncmp(text[which], value, TEXT_MAX - 1) == 0) {
        return;
    }
    strncpy(text[which], value, TEXT_MAX - 1);
    text[which][TEXT_MAX - 1] = 0;
    changed |= which == BTC_HF_OPERATOR ? BTC_CHANGED_PHONE : BTC_CHANGED_CALL;
}

static void update_call(void) {
    int c;
    if (call_on && st.held == 2) {
        c = BTC_CALL_HELD;
    } else if (call_on) {
        c = BTC_CALL_ACTIVE;
    } else if (callsetup == 1) {
        c = BTC_CALL_INCOMING;
    } else if (callsetup == 2) {
        c = BTC_CALL_DIALING;
    } else if (callsetup == 3) {
        c = BTC_CALL_ALERTING;
    } else {
        c = BTC_CALL_IDLE;
    }
    if (c != st.call) {
        st.call = c;
        if (c != BTC_CALL_INCOMING) {
            st.ringing = false;
        }
        changed |= BTC_CHANGED_CALL;
    }
}

// ---- call audio ----

static void voice_write(const int16_t *pcm, int n) {
    btc_ring_write(&ring, pcm, n, 1);
}

static void on_msbc_pcm(int16_t *data, int frames, int channels, int rate, void *context) {
    (void)channels;
    (void)rate;
    (void)context;
    voice_write(data, frames);
}

static void sco_receive(uint8_t *packet, uint16_t size) {
    if (size <= 3) {
        return;
    }
    if (codec == HFP_CODEC_MSBC) {
        btstack_sbc_decoder_process_data(&msbc_dec, (packet[1] >> 4) & 3, packet + 3, size - 3);
        return;
    }
    int16_t in[128], out[128];
    int n = (size - 3) / 2;
    if (n > 128) {
        n = 128;
    }
    for (int i = 0; i < n; i++) {
        in[i] = (int16_t)little_endian_read_16(packet, 3 + 2 * i);
    }
    bool bad = (packet[1] & 0x30) != 0;
    btstack_cvsd_plc_process_data(&cvsd_plc, bad, in, n, out);
    voice_write(out, n);
}

static uint32_t mic_take(int16_t *dst, uint32_t n) {
    uint32_t k = n < mic_count ? n : mic_count;
    for (uint32_t i = 0; i < k; i++) {
        dst[i] = mic[mic_head];
        mic_head = (mic_head + 1) % MIC_FRAMES;
    }
    mic_count -= k;
    return k;
}

static void sco_send(void) {
    if (sco == HCI_CON_HANDLE_INVALID) {
        return;
    }
    int len = hci_get_sco_packet_length();
    int payload = len - 3;
    hci_reserve_packet_buffer();
    uint8_t *p = hci_get_outgoing_packet_buffer();
    uint32_t prebuffer = st.sample_rate * MIC_PREBUFFER_MS / 1000;
    if (mic_paused && mic_count >= prebuffer) {
        mic_paused = false;
    }
    if (codec == HFP_CODEC_MSBC) {
        int need = hfp_codec_num_audio_samples_per_frame(&hfp_codec);
        if (!mic_paused && hfp_codec_can_encode_audio_frame_now(&hfp_codec)) {
            int16_t frame[120];
            if ((int)mic_count >= need) {
                mic_take(frame, need);
            } else {
                memset(frame, 0, sizeof(frame));
                mic_paused = true;
            }
            hfp_codec_encode_audio_frame(&hfp_codec, frame);
        } else if (mic_paused && hfp_codec_can_encode_audio_frame_now(&hfp_codec)) {
            int16_t frame[120];
            memset(frame, 0, sizeof(frame));
            hfp_codec_encode_audio_frame(&hfp_codec, frame);
        }
        if (hfp_codec_num_bytes_available(&hfp_codec) >= payload) {
            hfp_codec_read_from_stream(&hfp_codec, &p[3], payload);
        } else {
            memset(&p[3], 0, payload);
        }
    } else {
        int n = payload / 2;
        int16_t s[64];
        if (n > 64) {
            n = 64;
        }
        int got = mic_paused ? 0 : (int)mic_take(s, n);
        if (got < n) {
            memset(&s[got], 0, (n - got) * 2);
            if (!mic_paused) {
                mic_paused = true;
            }
        }
        for (int i = 0; i < n; i++) {
            little_endian_store_16(p, 3 + 2 * i, (uint16_t)s[i]);
        }
    }
    little_endian_store_16(p, 0, sco);
    p[2] = payload;
    hci_send_sco_packet_buffer(len);
    hci_request_sco_can_send_now_event();
}

static void on_sco(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (started && packet_type == HCI_SCO_DATA_PACKET && sco != HCI_CON_HANDLE_INVALID
        && (little_endian_read_16(packet, 0) & 0xfff) == sco) {
        sco_receive(packet, size);
    }
}

static void on_hci(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == HCI_EVENT_SCO_CAN_SEND_NOW) {
        sco_send();
    }
}

static void audio_open(uint8_t negotiated) {
    codec = negotiated;
    st.sample_rate = codec == HFP_CODEC_MSBC ? 16000 : 8000;
    uint32_t frames = st.sample_rate * ring_ms / 1000;
    btc_ring_set_limit(&ring, frames < MAX_RING_FRAMES ? frames : MAX_RING_FRAMES);
    mic_head = mic_count = 0;
    mic_paused = true;
    if (codec == HFP_CODEC_MSBC) {
        btstack_sbc_decoder_init(&msbc_dec, SBC_MODE_mSBC, on_msbc_pcm, NULL);
        hfp_codec_init_msbc(&hfp_codec, &msbc_enc);
    } else {
        btstack_cvsd_plc_init(&cvsd_plc);
    }
    st.audio = true;
    changed |= BTC_CHANGED_AUDIO;
    hci_request_sco_can_send_now_event();
}

static void audio_close(void) {
    if (codec == HFP_CODEC_MSBC) {
        hfp_codec_deinit(&hfp_codec);
    }
    sco = HCI_CON_HANDLE_INVALID;
    st.audio = false;
    st.sample_rate = 0;
    changed |= BTC_CHANGED_AUDIO;
}

// ---- HFP ----

static void indicator(const char *name, int value) {
    if (strcmp(name, "service") == 0) {
        st.service = value;
    } else if (strcmp(name, "signal") == 0) {
        st.signal = value;
    } else if (strcmp(name, "roam") == 0) {
        st.roaming = value;
    } else if (strcmp(name, "battchg") == 0) {
        st.battery = value;
    } else if (strcmp(name, "call") == 0) {
        call_on = value;
        update_call();
        return;
    } else if (strcmp(name, "callsetup") == 0) {
        callsetup = value;
        update_call();
        return;
    } else if (strcmp(name, "callheld") == 0) {
        st.held = value;
        update_call();
        return;
    } else {
        return;
    }
    changed |= BTC_CHANGED_PHONE;
}

static void on_hfp(uint8_t packet_type, uint16_t channel, uint8_t *event, uint16_t size) {
    (void)channel;
    (void)size;
    if (!started) {
        return;     // a link closing after stop
    }
    if (packet_type == HCI_SCO_DATA_PACKET) {
        on_sco(packet_type, channel, event, size);
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_HFP_META) {
        return;
    }
    bd_addr_t addr;
    switch (hci_event_hfp_meta_get_subevent_code(event)) {
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
            uint8_t status = hfp_subevent_service_level_connection_established_get_status(event);
            hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
            btc_log("hfp %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            acl = hfp_subevent_service_level_connection_established_get_acl_handle(event);
            memcpy(st.peer, addr, 6);
            st.connected = true;
            btc_radio_remember_peer(addr);
            hfp_hf_query_operator_selection(acl);
            hfp_hf_activate_calling_line_notification(acl);
            hfp_hf_activate_call_waiting_notification(acl);
            #ifdef ENABLE_HFP_DECLINE_SCO
            // Android's audio policy for this headset (AT+ANDROID, Android 14 and
            // later): don't make it the active device, don't move a call's audio
            // to it and don't ring in band. Without it, an app that asks for the
            // headset's audio makes the phone open a virtual call and try the
            // declined link again several times a second. Other phones answer
            // ERROR, which changes nothing.
            if (hfp_hf_send_at_command(acl, "AT+ANDROID=SINKAUDIOPOLICY,2,2,2\r") != ERROR_CODE_SUCCESS) {
                btc_log("hfp audio policy not sent");
            }
            #endif
            changed |= BTC_CHANGED_CONNECTION;
            break;
        }
        case HFP_SUBEVENT_CUSTOM_AT_MESSAGE_SENT:
            btc_log("hfp audio policy status 0x%02x", hfp_subevent_custom_at_message_sent_get_status(event));
            break;
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            btc_log("hfp released");
            acl = HCI_CON_HANDLE_INVALID;
            st.connected = false;
            changed |= BTC_CHANGED_CONNECTION;
            break;
        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED: {
            uint8_t status = hfp_subevent_audio_connection_established_get_status(event);
            uint8_t c = hfp_subevent_audio_connection_established_get_negotiated_codec(event);
            btc_log("call audio status 0x%02x codec %u rx %u tx %u", status, c,
                hfp_subevent_audio_connection_established_get_rx_packet_length(event),
                hfp_subevent_audio_connection_established_get_tx_packet_length(event));
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            sco = hfp_subevent_audio_connection_established_get_sco_handle(event);
            audio_open(c);
            break;
        }
        case HFP_SUBEVENT_AUDIO_CONNECTION_RELEASED:
            btc_log("call audio closed");
            audio_close();
            break;
        case HFP_SUBEVENT_AG_INDICATOR_MAPPING:
            break;
        case HFP_SUBEVENT_AG_INDICATOR_STATUS_CHANGED:
            indicator((const char *)hfp_subevent_ag_indicator_status_changed_get_indicator_name(event),
                hfp_subevent_ag_indicator_status_changed_get_indicator_status(event));
            break;
        case HFP_SUBEVENT_NETWORK_OPERATOR_CHANGED:
            set_text(BTC_HF_OPERATOR, (const char *)hfp_subevent_network_operator_changed_get_network_operator_name(event));
            break;
        case HFP_SUBEVENT_START_RINGING:
        case HFP_SUBEVENT_RING:
            if (!st.ringing) {
                st.ringing = true;
                changed |= BTC_CHANGED_CALL;
            }
            break;
        case HFP_SUBEVENT_STOP_RINGING:
            st.ringing = false;
            changed |= BTC_CHANGED_CALL;
            break;
        case HFP_SUBEVENT_CALLING_LINE_IDENTIFICATION_NOTIFICATION:
            set_text(BTC_HF_CALLER, (const char *)hfp_subevent_calling_line_identification_notification_get_number(event));
            break;
        case HFP_SUBEVENT_CALL_WAITING_NOTIFICATION:
            st.waiting = true;
            set_text(BTC_HF_CALLER, (const char *)hfp_subevent_call_waiting_notification_get_number(event));
            changed |= BTC_CHANGED_CALL;
            break;
        case HFP_SUBEVENT_SUBSCRIBER_NUMBER_INFORMATION:
            set_text(BTC_HF_SUBSCRIBER, (const char *)hfp_subevent_subscriber_number_information_get_bnip_number(event));
            break;
        case HFP_SUBEVENT_SPEAKER_VOLUME:
            st.speaker_gain = hfp_subevent_speaker_volume_get_gain(event);
            changed |= BTC_CHANGED_PHONE;
            break;
        case HFP_SUBEVENT_MICROPHONE_VOLUME:
            st.mic_gain = hfp_subevent_microphone_volume_get_gain(event);
            changed |= BTC_CHANGED_PHONE;
            break;
        case HFP_SUBEVENT_IN_BAND_RING_TONE:
            st.in_band_ring = hfp_subevent_in_band_ring_tone_get_status(event) != 0;
            changed |= BTC_CHANGED_PHONE;
            break;
        case HFP_SUBEVENT_VOICE_RECOGNITION_ACTIVATED:
            if (hfp_subevent_voice_recognition_activated_get_status(event) == ERROR_CODE_SUCCESS) {
                st.voice_recognition = true;
                changed |= BTC_CHANGED_PHONE;
            }
            break;
        case HFP_SUBEVENT_VOICE_RECOGNITION_DEACTIVATED:
            st.voice_recognition = false;
            changed |= BTC_CHANGED_PHONE;
            break;
        case HFP_SUBEVENT_CALL_TERMINATED:
            st.waiting = false;
            changed |= BTC_CHANGED_CALL;
            break;
        default:
            break;
    }
}

// ---- the API ----

int btc_hf_start(uint32_t ms, uint8_t channels) {
    if (started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (channels != 1 && channels != 2) {
        return BTC_ERR_ARG;
    }
    out_channels = channels;
    ring_ms = ms ? ms : 200;
    memset(&st, 0, sizeof(st));
    memset(text, 0, sizeof(text));
    st.service = st.signal = st.roaming = st.battery = BTC_UNKNOWN;
    st.speaker_gain = st.mic_gain = BTC_UNKNOWN;
    callsetup = call_on = 0;
    changed = 0;
    ring_buf = btc_btstack_alloc(MAX_RING_FRAMES * sizeof(int16_t));
    mic = btc_btstack_alloc(MIC_FRAMES * sizeof(int16_t));
    if (ring_buf == NULL || mic == NULL) {
        btc_btstack_free(ring_buf);
        btc_btstack_free(mic);
        ring_buf = mic = NULL;
        return BTC_ERR_NOMEM;
    }
    btc_ring_init(&ring, ring_buf, MAX_RING_FRAMES, 1);

    btc_rfcomm_init_once();
    static const uint16_t indicators[] = { 1, 2 };     // enhanced safety, battery level
    uint32_t features = (1 << HFP_HFSF_CLI_PRESENTATION_CAPABILITY)
        | (1 << HFP_HFSF_HF_INDICATORS) | (1 << HFP_HFSF_ENHANCED_CALL_STATUS)
        | (1 << HFP_HFSF_VOICE_RECOGNITION_FUNCTION)
        | (1 << HFP_HFSF_REMOTE_VOLUME_CONTROL) | (1 << HFP_HFSF_THREE_WAY_CALLING);
    #ifdef ENABLE_HFP_DECLINE_SCO
    // No call audio: offer no eSCO settings, no codec negotiation and no
    // wideband speech, so a phone has nothing more to try than plain CVSD.
    static const uint8_t codecs[] = { HFP_CODEC_CVSD };
    const int wideband = 0;
    #else
    static const uint8_t codecs[] = { HFP_CODEC_CVSD, HFP_CODEC_MSBC };
    const int wideband = 1;
    features |= (1 << HFP_HFSF_ESCO_S4) | (1 << HFP_HFSF_CODEC_NEGOTIATION);
    #endif
    // Every start: it registers the RFCOMM service stop took away, and its
    // HCI handler only once. Never hfp_hf_deinit(): it zeroes that handler's
    // registration without taking it off HCI's list, and the next HCI event
    // calls a NULL callback.
    hfp_hf_init(HF_RFCOMM_CHANNEL);
    hfp_hf_init_supported_features(features);
    hfp_hf_init_hf_indicators(sizeof(indicators) / sizeof(indicators[0]), indicators);
    hfp_hf_init_codecs(sizeof(codecs), codecs);
    hfp_hf_register_packet_handler(on_hfp);
    memset(sdp_hf, 0, sizeof(sdp_hf));
    hfp_hf_create_sdp_record(sdp_hf, sdp_create_service_record_handle(), HF_RFCOMM_CHANNEL,
        "Hands-Free", features & 0x1f, wideband);
    sdp_register_service(sdp_hf);

    hci_cb.callback = on_hci;
    hci_add_event_handler(&hci_cb);
    hci_register_sco_packet_handler(on_sco);
    started = true;
    btc_radio_profile(BTC_PROFILE_HF, true, 0);
    return BTC_OK;
}

void btc_hf_stop(void) {
    if (!started) {
        return;
    }
    btc_radio_profile(BTC_PROFILE_HF, false, 0);
    btc_hf_disconnect();
    sdp_unregister_service(big_endian_read_32(sdp_hf, 9));
    hci_remove_event_handler(&hci_cb);
    rfcomm_unregister_service(HF_RFCOMM_CHANNEL);
    started = false;
    sco = HCI_CON_HANDLE_INVALID;
    acl = HCI_CON_HANDLE_INVALID;
    btc_ring_init(&ring, NULL, 0, 1);
    btc_btstack_free(ring_buf);
    btc_btstack_free(mic);
    ring_buf = mic = NULL;
    mic_count = 0;
}

void btc_hf_radio_stopping(void) {
    btc_hf_stop();
}

bool btc_hf_started(void) {
    return started;
}

size_t btc_hf_read(int16_t *dst, size_t frames) {
    if (!st.audio) {
        memset(dst, 0, frames * out_channels * sizeof(int16_t));
        return 0;
    }
    // The call follows the phone's call volume.
    int32_t level = st.speaker_gain >= 0 ? st.speaker_gain * 127 / 15 : 127;
    return btc_ring_read(&ring, dst, frames, out_channels, level * level);
}

size_t btc_hf_write(const int16_t *pcm, size_t frames) {
    if (!st.audio || mic == NULL) {
        return 0;
    }
    size_t k = 0;
    while (k < frames && mic_count < MIC_FRAMES) {
        mic[(mic_head + mic_count) % MIC_FRAMES] = pcm[k++];
        mic_count++;
    }
    return k;
}

uint32_t btc_hf_changes(void) {
    uint32_t c = changed;
    changed = 0;
    return c;
}

void btc_hf_status(btc_hf_status_t *out) {
    *out = st;
}

void btc_hf_stats(btc_ring_stats_t *out) {
    btc_ring_stats(&ring, out);
}

size_t btc_hf_text(btc_hf_text_t which, char *dst, size_t size) {
    if ((unsigned)which >= BTC_HF_TEXT_COUNT || size == 0) {
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

static int result(uint8_t s) {
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

#define NEED_LINK() do { if (!started || acl == HCI_CON_HANDLE_INVALID) return BTC_ERR_STATE; } while (0)

int btc_hf_answer(void) {
    NEED_LINK();
    return result(hfp_hf_answer_incoming_call(acl));
}

int btc_hf_hangup(void) {
    NEED_LINK();
    if (st.call == BTC_CALL_INCOMING) {
        return result(hfp_hf_reject_incoming_call(acl));
    }
    return result(hfp_hf_terminate_call(acl));
}

int btc_hf_dial(const char *number) {
    NEED_LINK();
    if (number == NULL) {
        return result(hfp_hf_redial_last_number(acl));
    }
    return result(hfp_hf_dial_number(acl, (char *)number));
}

int btc_hf_dtmf(char digit) {
    NEED_LINK();
    return result(hfp_hf_send_dtmf_code(acl, digit));
}

int btc_hf_voice_recognition(bool on) {
    NEED_LINK();
    return result(on ? hfp_hf_activate_voice_recognition(acl) : hfp_hf_deactivate_voice_recognition(acl));
}

int btc_hf_set_speaker_gain(int gain) {
    NEED_LINK();
    if (gain < 0 || gain > 15) {
        return BTC_ERR_ARG;
    }
    st.speaker_gain = gain;
    return result(hfp_hf_set_speaker_gain(acl, gain));
}

int btc_hf_set_mic_gain(int gain) {
    NEED_LINK();
    if (gain < 0 || gain > 15) {
        return BTC_ERR_ARG;
    }
    st.mic_gain = gain;
    return result(hfp_hf_set_microphone_gain(acl, gain));
}

int btc_hf_connect_audio(void) {
    #ifdef ENABLE_HFP_DECLINE_SCO
    return BTC_ERR_UNSUPPORTED;     // the controller can't carry call audio: see radio.c
    #endif
    NEED_LINK();
    return result(hfp_hf_establish_audio_connection(acl));
}

int btc_hf_disconnect_audio(void) {
    NEED_LINK();
    return result(hfp_hf_release_audio_connection(acl));
}

int btc_hf_connect(const uint8_t *addr) {
    if (!started) {
        return BTC_ERR_STATE;
    }
    bd_addr_t peer;
    if (addr) {
        memcpy(peer, addr, 6);
    } else if (!btc_radio_last_peer(peer)) {
        return BTC_ERR_STATE;
    }
    return result(hfp_hf_establish_service_level_connection(peer));
}

int btc_hf_disconnect(void) {
    NEED_LINK();
    return result(hfp_hf_release_service_level_connection(acl));
}

void btc_hf_radio_lost(void) {
    started = false;
    btc_ring_init(&ring, NULL, 0, 1);
    ring_buf = mic = NULL;
    mic_count = 0;
    sco = HCI_CON_HANDLE_INVALID;
    acl = HCI_CON_HANDLE_INVALID;
    memset(&st, 0, sizeof(st));
}
