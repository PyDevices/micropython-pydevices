// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The sender's player settings (repeat, shuffle), read back by the AVRCP
// controller. AVRCP reports them through notification 0x08
// (PLAYER_APPLICATION_SETTING_CHANGED): the INTERIM reply to registering it
// carries every setting the player has, as it is now, and each CHANGED reply
// carries the ones that moved.
//
// Bluedroid's controller can't pass these on: it refuses to register 0x08,
// and its response handler drops the event's attribute/value pairs
// (esp_avrc_rn_param_t has no member for them; ESP-IDF 5.5 and 6.1 alike).
// So this file sits in front of it at link time:
//
//   --wrap=btc_avrc_ct_rn_evt_supported   the controller may register 0x08.
//   --wrap=AVRC_ParsResponse              every vendor response to the
//       controller passes through here; we read 0x08's pairs and hand the
//       response on unchanged.
//
// These are internal names, so the build turns this on only for the ESP-IDF
// versions it was written against (avrcp_settings.cmake); elsewhere the
// settings read as unknown.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/bt_target.h"
#include "stack/avrc_api.h"
#include "stack/avrc_defs.h"

#include "avrcp_settings.h"

bool __real_btc_avrc_ct_rn_evt_supported(uint8_t event_id);
tAVRC_STS __real_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result);

bool __wrap_btc_avrc_ct_rn_evt_supported(uint8_t event_id);
tAVRC_STS __wrap_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result);

bool __wrap_btc_avrc_ct_rn_evt_supported(uint8_t event_id) {
    return event_id == AVRC_EVT_APP_SETTING_CHANGE || __real_btc_avrc_ct_rn_evt_supported(event_id);
}

// A vendor response is: PDU, packet type, parameter length (2 bytes), then
// the parameters. For notification 0x08 those are the event ID, a count,
// and that many attribute/value pairs.
static void read_settings(const tAVRC_MSG_VENDOR *v) {
    const uint8_t *p = v->p_vendor_data;
    if (p == NULL || v->vendor_len < 6 || p[0] != AVRC_PDU_REGISTER_NOTIFICATION
        || p[4] != AVRC_EVT_APP_SETTING_CHANGE) {
        return;
    }
    bool interim = v->hdr.ctype == AVRC_RSP_INTERIM;
    if (!interim && v->hdr.ctype != AVRC_RSP_CHANGED) {
        return;
    }
    int len = (p[2] << 8) | p[3];
    if (len > v->vendor_len - 4) {
        len = v->vendor_len - 4;
    }
    int count = p[5];
    if (count > (len - 2) / 2) {
        count = (len - 2) / 2;
    }
    btc_avrcp_settings(&p[6], count < 0 ? 0 : count, interim);
}

tAVRC_STS __wrap_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result) {
    if (p_msg != NULL && p_msg->hdr.opcode == AVRC_OP_VENDOR) {
        read_settings(&p_msg->vendor);
    }
    return __real_AVRC_ParsResponse(p_msg, p_result);
}
