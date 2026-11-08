// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The AVRCP target's track metadata, for the audio source: a car stereo or
// headset asks the board what's playing (GetElementAttributes), where it is
// (GetPlayStatus), and to hear when either changes (RegisterNotification).
//
// Bluedroid's public API can't answer these: btc_avrc.c rejects the first
// two, and its target offers no notification but volume (ESP-IDF 5.5 and
// 6.1 alike). So this file sits in front of it at link time:
//
//   --wrap=btc_rc_handler                    every AVRCP event from BTA; we
//       answer those three for the source and hand everything else on.
//   --wrap=btc_avrc_tg_rn_evt_supported      the notifications the target
//   --wrap=btc_avrc_tg_get_rn_supported_evt  offers: play status, track and
//       position join volume while the source runs.
//
// The responses are built with Bluedroid's own AVRC_BldResponse and sent with
// BTA_AvMetaRsp, as btc_avrc.c does for volume. These are internal names, so
// the build turns this on (BTC_TG_METADATA) only for ESP-IDF versions it was
// written against; elsewhere the source works without metadata.

#include "btc_bluedroid.h"

#if BTC_TG_METADATA

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "common/bt_target.h"
#include "bta/bta_av_api.h"
#include "stack/avrc_api.h"

static const char *TAG = "btclassic";

// The events we answer, beside Bluedroid's volume (bit 13).
#define OUR_EVENTS ((1u << AVRC_EVT_PLAY_STATUS_CHANGE) | (1u << AVRC_EVT_TRACK_CHANGE) | (1u << AVRC_EVT_PLAY_POS_CHANGED))

void __real_btc_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data);
bool __real_btc_avrc_tg_rn_evt_supported(uint8_t event_id);
uint16_t __real_btc_avrc_tg_get_rn_supported_evt(void);

void __wrap_btc_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data);
bool __wrap_btc_avrc_tg_rn_evt_supported(uint8_t event_id);
uint16_t __wrap_btc_avrc_tg_get_rn_supported_evt(void);

// Who asked to hear of what: the label to answer with, per event.
static struct {
    bool open;
    uint8_t rc_handle;
    bool registered[AVRC_EVT_PLAY_POS_CHANGED + 1];
    uint8_t label[AVRC_EVT_PLAY_POS_CHANGED + 1];
} t;

static bool ours(uint8_t event_id) {
    return event_id < 16 && (OUR_EVENTS >> event_id) & 1;
}

bool __wrap_btc_avrc_tg_rn_evt_supported(uint8_t event_id) {
    if (btc_source_tg_active() && ours(event_id)) {
        return true;
    }
    return __real_btc_avrc_tg_rn_evt_supported(event_id);
}

uint16_t __wrap_btc_avrc_tg_get_rn_supported_evt(void) {
    uint16_t bits = __real_btc_avrc_tg_get_rn_supported_evt();
    return btc_source_tg_active() ? bits | OUR_EVENTS : bits;
}

void btc_tg_metadata_reset(void) {
    BTC_LOCK();
    memset(t.registered, 0, sizeof(t.registered));
    BTC_UNLOCK();
}

// ---- building responses ----

static void send(uint8_t handle, uint8_t label, tBTA_AV_CODE ctype, tAVRC_RESPONSE *rsp) {
    BT_HDR *pkt = NULL;
    tAVRC_STS st = AVRC_BldResponse(handle, rsp, &pkt);
    if (st == AVRC_STS_NO_ERROR && pkt) {
        BTA_AvMetaRsp(handle, label, ctype, pkt);
    } else {
        ESP_LOGW(TAG, "AVRCP response 0x%02x not built: %d", rsp->pdu, st);
    }
}

static void fill_notify(tAVRC_RESPONSE *rsp, uint8_t event_id, const btc_tg_track_t *tr) {
    memset(rsp, 0, sizeof(*rsp));
    rsp->reg_notif.pdu = AVRC_PDU_REGISTER_NOTIFICATION;
    rsp->reg_notif.opcode = AVRC_OP_VENDOR;
    rsp->reg_notif.status = AVRC_STS_NO_ERROR;
    rsp->reg_notif.event_id = event_id;
    switch (event_id) {
        case AVRC_EVT_PLAY_STATUS_CHANGE:
            rsp->reg_notif.param.play_status = (tAVRC_PLAYSTATE)tr->play_status;
            break;
        case AVRC_EVT_TRACK_CHANGE:
            // AVRCP 1.3: 0 is "a track is selected", all ones "none".
            memset(rsp->reg_notif.param.track, tr->title[0] ? 0x00 : 0xff, sizeof(rsp->reg_notif.param.track));
            break;
        case AVRC_EVT_PLAY_POS_CHANGED:
            rsp->reg_notif.param.play_pos = tr->position_ms >= 0 ? (UINT32)tr->position_ms : 0xffffffff;
            break;
    }
}

static void add_attr(tAVRC_ATTR_ENTRY *e, uint32_t id, const char *text) {
    e->attr_id = id;
    e->name.charset_id = AVRC_CHARSET_ID_UTF8;
    e->name.str_len = (UINT16)strlen(text);
    e->name.p_str = (UINT8 *)text;
}

static void answer_elements(uint8_t handle, uint8_t label, const tAVRC_GET_ELEM_ATTRS_CMD *cmd) {
    btc_tg_track_t tr;
    btc_source_tg_track(&tr);
    char num[12], count[12], time[24];
    snprintf(num, sizeof(num), "%d", tr.track_number > 0 ? tr.track_number : 0);
    snprintf(count, sizeof(count), "%d", tr.track_count > 0 ? tr.track_count : 0);
    snprintf(time, sizeof(time), "%lld", (long long)(tr.duration_ms > 0 ? tr.duration_ms : 0));

    uint32_t want[AVRC_MAX_NUM_MEDIA_ATTR_ID];
    int n_want = cmd->num_attr;
    if (n_want == 0 || n_want > AVRC_MAX_NUM_MEDIA_ATTR_ID) {
        n_want = AVRC_MAX_NUM_MEDIA_ATTR_ID;    // none named: all of them
        for (int i = 0; i < n_want; i++) {
            want[i] = i + 1;
        }
    } else {
        for (int i = 0; i < n_want; i++) {
            want[i] = cmd->attrs[i];
        }
    }
    tAVRC_ATTR_ENTRY attrs[AVRC_MAX_NUM_MEDIA_ATTR_ID];
    int n = 0;
    for (int i = 0; i < n_want; i++) {
        const char *text = NULL;
        switch (want[i]) {
            case AVRC_MEDIA_ATTR_ID_TITLE:
                text = tr.title;
                break;
            case AVRC_MEDIA_ATTR_ID_ARTIST:
                text = tr.artist;
                break;
            case AVRC_MEDIA_ATTR_ID_ALBUM:
                text = tr.album;
                break;
            case AVRC_MEDIA_ATTR_ID_TRACK_NUM:
                text = tr.track_number > 0 ? num : "";
                break;
            case AVRC_MEDIA_ATTR_ID_NUM_TRACKS:
                text = tr.track_count > 0 ? count : "";
                break;
            case AVRC_MEDIA_ATTR_ID_GENRE:
                text = tr.genre;
                break;
            case AVRC_MEDIA_ATTR_ID_PLAYING_TIME:
                text = tr.duration_ms > 0 ? time : "";
                break;
        }
        if (text) {
            add_attr(&attrs[n++], want[i], text);
        }
    }
    tAVRC_RESPONSE rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.get_elem_attrs.pdu = AVRC_PDU_GET_ELEMENT_ATTR;
    rsp.get_elem_attrs.opcode = AVRC_OP_VENDOR;
    rsp.get_elem_attrs.status = AVRC_STS_NO_ERROR;
    rsp.get_elem_attrs.num_attr = (UINT8)n;
    rsp.get_elem_attrs.p_attrs = attrs;
    send(handle, label, AVRC_RSP_IMPL_STBL, &rsp);
}

static void answer_play_status(uint8_t handle, uint8_t label) {
    btc_tg_track_t tr;
    btc_source_tg_track(&tr);
    tAVRC_RESPONSE rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.get_play_status.pdu = AVRC_PDU_GET_PLAY_STATUS;
    rsp.get_play_status.opcode = AVRC_OP_VENDOR;
    rsp.get_play_status.status = AVRC_STS_NO_ERROR;
    rsp.get_play_status.song_len = tr.duration_ms > 0 ? (UINT32)tr.duration_ms : 0xffffffff;
    rsp.get_play_status.song_pos = tr.position_ms >= 0 ? (UINT32)tr.position_ms : 0xffffffff;
    rsp.get_play_status.play_status = (UINT8)tr.play_status;
    send(handle, label, AVRC_RSP_IMPL_STBL, &rsp);
}

static void answer_register(uint8_t handle, uint8_t label, uint8_t event_id) {
    BTC_LOCK();
    t.registered[event_id] = true;
    t.label[event_id] = label;
    t.rc_handle = handle;
    BTC_UNLOCK();
    btc_tg_track_t tr;
    btc_source_tg_track(&tr);
    tAVRC_RESPONSE rsp;
    fill_notify(&rsp, event_id, &tr);
    send(handle, label, AVRC_RSP_INTERIM, &rsp);
}

void btc_tg_metadata_changed(uint8_t event_id) {
    if (!ours(event_id)) {
        return;
    }
    BTC_LOCK();
    bool go = t.open && t.registered[event_id];
    uint8_t label = t.label[event_id];
    uint8_t handle = t.rc_handle;
    t.registered[event_id] = false;     // a CHANGED answers the registration
    BTC_UNLOCK();
    if (!go) {
        return;
    }
    btc_tg_track_t tr;
    btc_source_tg_track(&tr);
    tAVRC_RESPONSE rsp;
    fill_notify(&rsp, event_id, &tr);
    send(handle, label, AVRC_RSP_CHANGED, &rsp);
}

// A command for us, answered: true. Anything else goes on to Bluedroid.
static bool handle_meta(tBTA_AV_META_MSG *m) {
    if (m->p_msg == NULL || m->p_msg->hdr.opcode != AVRC_OP_VENDOR || m->code >= AVRC_RSP_NOT_IMPL || m->len < 3) {
        return false;
    }
    static uint8_t buf[512];    // Bluedroid's task only
    tAVRC_COMMAND cmd;
    memset(&cmd, 0, sizeof(cmd));
    if (AVRC_ParsCommand(m->p_msg, &cmd, buf, sizeof(buf)) != AVRC_STS_NO_ERROR) {
        return false;
    }
    switch (cmd.pdu) {
        case AVRC_PDU_GET_ELEMENT_ATTR:
            answer_elements(m->rc_handle, m->label, &cmd.get_elem_attrs);
            return true;
        case AVRC_PDU_GET_PLAY_STATUS:
            answer_play_status(m->rc_handle, m->label);
            return true;
        case AVRC_PDU_REGISTER_NOTIFICATION:
            if (ours(cmd.reg_notif.event_id)) {
                answer_register(m->rc_handle, m->label, cmd.reg_notif.event_id);
                return true;
            }
            return false;
        default:
            return false;
    }
}

void __wrap_btc_rc_handler(tBTA_AV_EVT event, tBTA_AV *p_data) {
    if (btc_source_tg_active() && p_data) {
        switch (event) {
            case BTA_AV_RC_OPEN_EVT:
                btc_tg_metadata_reset();
                t.open = true;
                t.rc_handle = p_data->rc_open.rc_handle;
                break;
            case BTA_AV_RC_CLOSE_EVT:
                btc_tg_metadata_reset();
                t.open = false;
                break;
            case BTA_AV_META_MSG_EVT:
                if (handle_meta(&p_data->meta_msg)) {
                    return;
                }
                break;
            default:
                break;
        }
    }
    __real_btc_rc_handler(event, p_data);
}

#endif // BTC_TG_METADATA
