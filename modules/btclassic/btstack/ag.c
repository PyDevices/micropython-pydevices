// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's audio gateway on BTstack: HFP AG over RFCOMM, the board as the
// phone. A headset connects and sees a network, a signal and a battery; the
// board rings it, and it answers, hangs up, dials and sets the volume.
//
// Control only. The CYW43439's firmware never completes a SCO link
// (raspberrypi/pico-sdk#1461), so the build declines call audio (radio.c) and
// this gateway offers none: no eSCO, no codec negotiation, no in-band ring.
// The call's audio stays wherever the program puts it.
//
// BTstack keeps the call state itself, in the model behind hfp_gsm_*. What
// the board reports is read back from it after every action and event, so
// the headset's own buttons and the board's calls agree.

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

#define AG_RFCOMM_CHANNEL 1
#define TEXT_MAX 64
#define DTMF_MAX 32

static bool started;
static uint32_t changed;
static btc_ag_status_t st;
static char text[BTC_AG_TEXT_COUNT][TEXT_MAX];
static char dtmf[DTMF_MAX];
static uint8_t dtmf_len;
static uint8_t sdp_ag[150];
static hci_con_handle_t acl = HCI_CON_HANDLE_INVALID;
static bd_addr_t last;
static bool has_last;
static hfp_phone_number_t subscriber;

static void set_text(btc_ag_text_t which, const char *value) {
    strncpy(text[which], value ? value : "", TEXT_MAX - 1);
    text[which][TEXT_MAX - 1] = 0;
}

static uint8_t number_type(const char *number) {
    return number[0] == '+' ? 145 : 129;
}

// The operator the headset reads with AT+COPS?: BTstack keeps it in the
// connection and has no call to set it.
static void apply_operator(void) {
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, hfp_get_connections());
    while (btstack_linked_list_iterator_has_next(&it)) {
        hfp_connection_t *c = (hfp_connection_t *)btstack_linked_list_iterator_next(&it);
        if (c->local_role == HFP_ROLE_AG) {
            strncpy(c->network_operator.name, text[BTC_AG_OPERATOR], HFP_MAX_NETWORK_OPERATOR_NAME_SIZE - 1);
            c->network_operator.name[HFP_MAX_NETWORK_OPERATOR_NAME_SIZE - 1] = 0;
        }
    }
}

static void apply_subscriber(void) {
    subscriber.type = number_type(text[BTC_AG_NUMBER]);
    subscriber.number = text[BTC_AG_NUMBER];
    hfp_ag_set_subcriber_number_information(&subscriber, text[BTC_AG_NUMBER][0] ? 1 : 0);
}

static void update_call(void) {
    int c = BTC_CALL_IDLE;
    if (hfp_gsm_call_status() == HFP_CALL_STATUS_ACTIVE_OR_HELD_CALL_IS_PRESENT) {
        c = hfp_gsm_callheld_status() == HFP_CALLHELD_STATUS_CALL_ON_HOLD_AND_NO_ACTIVE_CALLS
            ? BTC_CALL_HELD : BTC_CALL_ACTIVE;
    } else {
        switch (hfp_gsm_callsetup_status()) {
            case HFP_CALLSETUP_STATUS_INCOMING_CALL_SETUP_IN_PROGRESS:
                c = BTC_CALL_INCOMING;
                break;
            case HFP_CALLSETUP_STATUS_OUTGOING_CALL_SETUP_IN_DIALING_STATE:
                c = BTC_CALL_DIALING;
                break;
            case HFP_CALLSETUP_STATUS_OUTGOING_CALL_SETUP_IN_ALERTING_STATE:
                c = BTC_CALL_ALERTING;
                break;
            default:
                break;
        }
    }
    if (c != st.call) {
        st.call = c;
        changed |= BTC_CHANGED_CALL;
    }
}

static void on_hfp(uint8_t packet_type, uint16_t channel, uint8_t *event, uint16_t size) {
    (void)channel;
    (void)size;
    if (!started || packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(event) != HCI_EVENT_HFP_META) {
        return;
    }
    bd_addr_t addr;
    switch (hci_event_hfp_meta_get_subevent_code(event)) {
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_ESTABLISHED: {
            uint8_t status = hfp_subevent_service_level_connection_established_get_status(event);
            hfp_subevent_service_level_connection_established_get_bd_addr(event, addr);
            btc_log("hfp ag %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            acl = hfp_subevent_service_level_connection_established_get_acl_handle(event);
            memcpy(st.peer, addr, 6);
            memcpy(last, addr, 6);
            has_last = true;
            st.connected = true;
            apply_operator();
            changed |= BTC_CHANGED_CONNECTION;
            break;
        }
        case HFP_SUBEVENT_SERVICE_LEVEL_CONNECTION_RELEASED:
            btc_log("hfp ag released");
            acl = HCI_CON_HANDLE_INVALID;
            st.connected = false;
            st.speaker_gain = st.mic_gain = BTC_UNKNOWN;
            st.voice_recognition = false;
            changed |= BTC_CHANGED_CONNECTION;
            break;
        case HFP_SUBEVENT_CALL_ANSWERED:
        case HFP_SUBEVENT_CALL_TERMINATED:
            update_call();
            break;
        case HFP_SUBEVENT_PLACE_CALL_WITH_NUMBER: {
            // The headset dialed. A memory slot (">1") isn't kept, nor is a
            // second call.
            const char *number = hfp_subevent_place_call_with_number_get_number(event);
            if (number[0] == 0 || number[0] == '>' || st.call != BTC_CALL_IDLE) {
                hfp_ag_outgoing_call_rejected();
                break;
            }
            set_text(BTC_AG_DIALED, number);
            set_text(BTC_AG_CALLER, number);
            hfp_ag_set_last_dialed_number(number);
            hfp_ag_outgoing_call_accepted();
            update_call();
            changed |= BTC_CHANGED_REQUEST;
            break;
        }
        case HFP_SUBEVENT_TRANSMIT_DTMF_CODES: {
            const char *code = hfp_subevent_transmit_dtmf_codes_get_dtmf(event);
            for (; *code && dtmf_len < DTMF_MAX; code++) {
                dtmf[dtmf_len++] = *code;
            }
            changed |= BTC_CHANGED_REQUEST;
            hfp_ag_send_dtmf_code_done(acl);
            break;
        }
        case HFP_SUBEVENT_ATTACH_NUMBER_TO_VOICE_TAG:
            hfp_ag_reject_phone_number_for_voice_tag(acl);
            break;
        case HFP_SUBEVENT_SPEAKER_VOLUME:
            st.speaker_gain = hfp_subevent_speaker_volume_get_gain(event);
            changed |= BTC_CHANGED_PHONE;
            break;
        case HFP_SUBEVENT_MICROPHONE_VOLUME:
            st.mic_gain = hfp_subevent_microphone_volume_get_gain(event);
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
        case HFP_SUBEVENT_AUDIO_CONNECTION_ESTABLISHED:
            btc_log("hfp ag call audio status 0x%02x",
                hfp_subevent_audio_connection_established_get_status(event));
            break;
        default:
            break;
    }
}

// ---- the API ----

int btc_ag_start(uint32_t ring_ms) {
    (void)ring_ms;      // no call audio, so no buffers to size
    if (started || !btc_radio_active() || btc_hf_started()) {
        return BTC_ERR_STATE;
    }
    // Settings made before a start carry over, and the last headset.
    char op[TEXT_MAX], num[TEXT_MAX];
    strcpy(op, text[BTC_AG_OPERATOR][0] ? text[BTC_AG_OPERATOR] : "MicroPython");
    strcpy(num, text[BTC_AG_NUMBER]);
    memset(&st, 0, sizeof(st));
    memset(text, 0, sizeof(text));
    strcpy(text[BTC_AG_OPERATOR], op);
    strcpy(text[BTC_AG_NUMBER], num);
    st.service = 1;
    st.signal = 5;
    st.roaming = 0;
    st.battery = 5;
    st.call = BTC_CALL_IDLE;
    st.speaker_gain = st.mic_gain = BTC_UNKNOWN;
    dtmf_len = 0;
    changed = 0;

    btc_rfcomm_init_once();
    static const hfp_ag_indicator_t indicators[] = {
        // index, name, min, max, status, mandatory, enabled, status changed
        {1, "service", 0, 1, 1, 0, 0, 0},
        {2, "call", 0, 1, 0, 1, 1, 0},
        {3, "callsetup", 0, 3, 0, 1, 1, 0},
        {4, "battchg", 0, 5, 5, 0, 0, 0},
        {5, "signal", 0, 5, 5, 0, 1, 0},
        {6, "roam", 0, 1, 0, 0, 1, 0},
        {7, "callheld", 0, 2, 0, 1, 1, 0},
    };
    // No eSCO, codec negotiation, in-band ring tone or three-way calling:
    // see the top of this file.
    const uint16_t features = (1 << HFP_AGSF_EXTENDED_ERROR_RESULT_CODES)
        | (1 << HFP_AGSF_ENHANCED_CALL_STATUS) | (1 << HFP_AGSF_ABILITY_TO_REJECT_A_CALL)
        | (1 << HFP_AGSF_VOICE_RECOGNITION_FUNCTION);
    static const uint8_t codecs[] = { HFP_CODEC_CVSD };
    // Every start, like the hands-free unit: it registers the RFCOMM service
    // stop took away. Never hfp_ag_deinit(), for the reason hf.c gives.
    hfp_ag_init(AG_RFCOMM_CHANNEL);
    hfp_ag_init_supported_features(features);
    hfp_ag_init_codecs(sizeof(codecs), codecs);
    hfp_ag_init_ag_indicators(sizeof(indicators) / sizeof(indicators[0]), indicators);
    hfp_ag_set_use_in_band_ring_tone(0);
    apply_subscriber();
    hfp_ag_register_packet_handler(on_hfp);
    memset(sdp_ag, 0, sizeof(sdp_ag));
    hfp_ag_create_sdp_record(sdp_ag, sdp_create_service_record_handle(), AG_RFCOMM_CHANNEL,
        "Voice Gateway", 1, features, 0);
    sdp_register_service(sdp_ag);
    started = true;
    btc_radio_profile(BTC_PROFILE_AG, true, 0);
    return BTC_OK;
}

void btc_ag_stop(void) {
    if (!started) {
        return;
    }
    btc_radio_profile(BTC_PROFILE_AG, false, 0);
    btc_ag_disconnect();
    sdp_unregister_service(big_endian_read_32(sdp_ag, 9));
    rfcomm_unregister_service(AG_RFCOMM_CHANNEL);
    started = false;
    acl = HCI_CON_HANDLE_INVALID;
    st.connected = false;
}

void btc_ag_radio_stopping(void) {
    btc_ag_stop();
}

void btc_ag_radio_lost(void) {
    started = false;
    acl = HCI_CON_HANDLE_INVALID;
    memset(&st, 0, sizeof(st));
}

bool btc_ag_started(void) {
    return started;
}

static int result(uint8_t s) {
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

static int ready(void) {
    return started && st.connected && acl != HCI_CON_HANDLE_INVALID ? BTC_OK : BTC_ERR_STATE;
}

int btc_ag_connect(const uint8_t *addr) {
    if (!started) {
        return BTC_ERR_STATE;
    }
    bd_addr_t peer;
    if (addr) {
        memcpy(peer, addr, 6);
    } else if (has_last) {
        memcpy(peer, last, 6);
    } else {
        return BTC_ERR_STATE;
    }
    return result(hfp_ag_establish_service_level_connection(peer));
}

int btc_ag_disconnect(void) {
    int err = ready();
    return err ? err : result(hfp_ag_release_service_level_connection(acl));
}

int btc_ag_connect_audio(void) {
    return BTC_ERR_UNSUPPORTED;     // the controller can't carry call audio: see radio.c
}

int btc_ag_disconnect_audio(void) {
    return BTC_ERR_UNSUPPORTED;
}

// ---- what the gateway reports ----

static void report(int *field, int value, int lo, int hi, uint8_t (*send)(int)) {
    if (value == BTC_UNKNOWN) {
        return;
    }
    value = value < lo ? lo : value > hi ? hi : value;
    if (value != *field) {
        *field = value;
        send(value);
    }
}

int btc_ag_set_indicators(int service, int signal, int roaming, int battery) {
    if (!started) {
        return BTC_ERR_STATE;
    }
    report(&st.service, service, 0, 1, hfp_ag_set_registration_status);
    report(&st.signal, signal, 0, 5, hfp_ag_set_signal_strength);
    report(&st.roaming, roaming, 0, 1, hfp_ag_set_roaming_status);
    report(&st.battery, battery, 0, 5, hfp_ag_set_battery_level);
    update_call();      // losing service drops a call
    return BTC_OK;
}

int btc_ag_set_text(btc_ag_text_t which, const char *value) {
    if (which != BTC_AG_OPERATOR && which != BTC_AG_NUMBER) {
        return BTC_ERR_ARG;
    }
    set_text(which, value);
    if (started) {
        apply_operator();
        apply_subscriber();
    }
    return BTC_OK;
}

// ---- calls ----

int btc_ag_ring(const char *number) {
    int err = ready();
    if (err) {
        return err;
    }
    if (st.call != BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    set_text(BTC_AG_CALLER, number);
    hfp_ag_set_clip(number_type(text[BTC_AG_CALLER]), text[BTC_AG_CALLER]);
    hfp_ag_incoming_call();
    update_call();
    return st.call == BTC_CALL_INCOMING ? BTC_OK : BTC_ERR_FAIL;
}

int btc_ag_dial(const char *number) {
    int err = ready();
    if (err) {
        return err;
    }
    if (number == NULL || !number[0]) {
        return BTC_ERR_ARG;
    }
    if (st.call != BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    set_text(BTC_AG_DIALED, number);
    set_text(BTC_AG_CALLER, number);
    hfp_ag_set_last_dialed_number(number);
    hfp_ag_outgoing_call_initiated();
    // BTstack names the call "1234567"; the headset's call list should say
    // what was dialed.
    for (int i = 0; i < 10; i++) {
        hfp_gsm_call_t *c = hfp_gsm_call(i);
        if (c != NULL && c->used_slot && c->direction == HFP_ENHANCED_CALL_DIR_OUTGOING) {
            strncpy(c->clip_number, number, sizeof(c->clip_number) - 1);
            c->clip_number[sizeof(c->clip_number) - 1] = 0;
            c->clip_type = number_type(number);
        }
    }
    update_call();
    return st.call == BTC_CALL_DIALING ? BTC_OK : BTC_ERR_FAIL;
}

int btc_ag_alerting(void) {
    int err = ready();
    if (err) {
        return err;
    }
    if (st.call != BTC_CALL_DIALING) {
        return BTC_ERR_STATE;
    }
    hfp_ag_outgoing_call_ringing();
    update_call();
    return st.call == BTC_CALL_ALERTING ? BTC_OK : BTC_ERR_FAIL;
}

int btc_ag_answer(void) {
    int err = ready();
    if (err) {
        return err;
    }
    if (st.call == BTC_CALL_INCOMING) {
        hfp_ag_answer_incoming_call();
    } else if (st.call == BTC_CALL_DIALING || st.call == BTC_CALL_ALERTING) {
        hfp_ag_outgoing_call_established();
    } else {
        return BTC_ERR_STATE;
    }
    update_call();
    return st.call == BTC_CALL_ACTIVE ? BTC_OK : BTC_ERR_FAIL;
}

int btc_ag_hangup(void) {
    int err = ready();
    if (err) {
        return err;
    }
    if (st.call == BTC_CALL_IDLE) {
        return BTC_ERR_STATE;
    }
    // Ends an active call, and rejects one that is still ringing or dialing.
    hfp_ag_terminate_call();
    update_call();
    return BTC_OK;
}

int btc_ag_set_in_band_ring(bool on) {
    if (!started) {
        return BTC_ERR_STATE;
    }
    // An in-band ring tone plays through the call's audio.
    return on ? BTC_ERR_UNSUPPORTED : BTC_OK;
}

int btc_ag_voice_recognition(bool on) {
    int err = ready();
    if (err) {
        return err;
    }
    err = result(on ? hfp_ag_activate_voice_recognition(acl) : hfp_ag_deactivate_voice_recognition(acl));
    if (err == BTC_OK) {
        st.voice_recognition = on;
        changed |= BTC_CHANGED_PHONE;
    }
    return err;
}

static int set_gain(int *field, int gain, uint8_t (*send)(hci_con_handle_t, int)) {
    int err = ready();
    if (err) {
        return err;
    }
    if (gain < 0 || gain > 15) {
        return BTC_ERR_ARG;
    }
    err = result(send(acl, gain));
    if (err == BTC_OK && *field != gain) {
        *field = gain;
        changed |= BTC_CHANGED_PHONE;
    }
    return err;
}

int btc_ag_set_speaker_gain(int gain) {
    return set_gain(&st.speaker_gain, gain, hfp_ag_set_speaker_gain);
}

int btc_ag_set_mic_gain(int gain) {
    return set_gain(&st.mic_gain, gain, hfp_ag_set_microphone_gain);
}

size_t btc_ag_dtmf(char *dst, size_t size) {
    size_t n = dtmf_len < size ? dtmf_len : size;
    memcpy(dst, dtmf, n);
    memmove(dtmf, dtmf + n, dtmf_len - n);
    dtmf_len -= n;
    return n;
}

// ---- audio: none on this controller ----

size_t btc_ag_read(int16_t *dst, size_t frames, uint8_t channels) {
    memset(dst, 0, frames * (channels == 2 ? 2 : 1) * sizeof(int16_t));
    return 0;
}

size_t btc_ag_write(const int16_t *pcm, size_t frames) {
    (void)pcm;
    (void)frames;
    return 0;
}

// ---- state ----

uint32_t btc_ag_changes(void) {
    if (started) {
        update_call();
    }
    uint32_t c = changed;
    changed = 0;
    return c;
}

void btc_ag_status(btc_ag_status_t *out) {
    if (started) {
        update_call();
    }
    *out = st;
}

void btc_ag_stats(btc_ring_stats_t *out) {
    memset(out, 0, sizeof(*out));
}

size_t btc_ag_text(btc_ag_text_t which, char *dst, size_t size) {
    if ((unsigned)which >= BTC_AG_TEXT_COUNT || size == 0) {
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
