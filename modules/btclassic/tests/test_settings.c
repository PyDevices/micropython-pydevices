// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// avrcp_settings.c on the desktop: the sender's repeat and shuffle read out
// of AVRCP responses as Bluedroid hands them over, and everything else left
// alone. Bluedroid's names come from tests/stand_in.
//
// From modules/btclassic:
//   cc -std=c11 -Wall -Wextra -Werror -Itests/stand_in -Iesp-idf/btclassic
//      -o /tmp/test_settings tests/test_settings.c esp-idf/btclassic/avrcp_settings.c

#include <stdio.h>
#include <string.h>

#include "common/bt_target.h"
#include "stack/avrc_api.h"
#include "avrcp_settings.h"

bool __wrap_btc_avrc_ct_rn_evt_supported(uint8_t event_id);
tAVRC_STS __wrap_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result);

// Bluedroid's own controller allows play status, track, position and volume.
bool __real_btc_avrc_ct_rn_evt_supported(uint8_t event_id) {
    return (0x2026 >> event_id) & 1;
}

static int parsed;

tAVRC_STS __real_AVRC_ParsResponse(tAVRC_MSG *p_msg, tAVRC_RESPONSE *p_result) {
    (void)p_msg;
    (void)p_result;
    parsed++;
    return 0;
}

// What the sink was handed.
static struct {
    int calls;
    int count;
    bool complete;
    uint8_t pairs[16];
} got;

void btc_avrcp_settings(const uint8_t *pairs, int count, bool complete) {
    got.calls++;
    got.count = count;
    got.complete = complete;
    size_t n = (size_t)count * 2;
    memcpy(got.pairs, pairs, n <= sizeof(got.pairs) ? n : sizeof(got.pairs));
}

static int failures;

static void check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

// Hand a vendor response through the wrapper, as Bluedroid would.
static void respond(uint8_t ctype, const uint8_t *data, uint16_t len) {
    uint8_t copy[64];
    memcpy(copy, data, len);
    tAVRC_MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.vendor.hdr.opcode = AVRC_OP_VENDOR;
    msg.vendor.hdr.ctype = ctype;
    msg.vendor.p_vendor_data = copy;
    msg.vendor.vendor_len = len;
    tAVRC_RESPONSE rsp;
    memset(&got, 0, sizeof(got));
    int before = parsed;
    __wrap_AVRC_ParsResponse(&msg, &rsp);
    check(parsed == before + 1, "the response goes on to Bluedroid's parser");
}

int main(void) {
    // The controller may now register 0x08, and still everything it could.
    check(__wrap_btc_avrc_ct_rn_evt_supported(AVRC_EVT_APP_SETTING_CHANGE), "0x08 can be registered");
    check(__wrap_btc_avrc_ct_rn_evt_supported(AVRC_EVT_PLAY_STATUS_CHANGE), "play status still can");
    check(!__wrap_btc_avrc_ct_rn_evt_supported(0x07), "0x07 still can't");

    // The INTERIM reply: repeat all, shuffle off, the whole set.
    static const uint8_t interim[] = {0x31, 0x00, 0x00, 0x06, 0x08, 0x02, 0x02, 0x03, 0x03, 0x01};
    respond(AVRC_RSP_INTERIM, interim, sizeof(interim));
    check(got.calls == 1 && got.complete && got.count == 2, "INTERIM hands on both settings, complete");
    check(got.pairs[0] == 2 && got.pairs[1] == 3 && got.pairs[2] == 3 && got.pairs[3] == 1, "INTERIM pairs as sent");

    // A CHANGED reply with just shuffle.
    static const uint8_t changed[] = {0x31, 0x00, 0x00, 0x04, 0x08, 0x01, 0x03, 0x02};
    respond(AVRC_RSP_CHANGED, changed, sizeof(changed));
    check(got.calls == 1 && !got.complete && got.count == 1, "CHANGED hands on the one that moved");
    check(got.pairs[0] == 3 && got.pairs[1] == 2, "CHANGED pair as sent");

    // A count larger than the bytes that follow is cut to what's there.
    static const uint8_t short_one[] = {0x31, 0x00, 0x00, 0x04, 0x08, 0x05, 0x02, 0x01};
    respond(AVRC_RSP_CHANGED, short_one, sizeof(short_one));
    check(got.calls == 1 && got.count == 1, "a count past the end is cut");

    // A parameter length past the message is cut too.
    static const uint8_t long_len[] = {0x31, 0x00, 0x00, 0x40, 0x08, 0x02, 0x02, 0x01};
    respond(AVRC_RSP_INTERIM, long_len, sizeof(long_len));
    check(got.calls == 1 && got.count == 1, "a length past the message is cut");

    // Other events, other PDUs, other response types and runts: not ours.
    static const uint8_t play[] = {0x31, 0x00, 0x00, 0x02, 0x01, 0x01};
    respond(AVRC_RSP_CHANGED, play, sizeof(play));
    check(got.calls == 0, "play status is left alone");
    static const uint8_t status[] = {0x30, 0x00, 0x00, 0x09, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    respond(AVRC_RSP_ACCEPT, status, sizeof(status));
    check(got.calls == 0, "another PDU is left alone");
    respond(AVRC_RSP_ACCEPT, interim, sizeof(interim));
    check(got.calls == 0, "a response that is neither INTERIM nor CHANGED is left alone");
    respond(AVRC_RSP_INTERIM, interim, 5);
    check(got.calls == 0, "a runt is left alone");

    if (failures) {
        printf("%d failed\n", failures);
        return 1;
    }
    printf("ok: player settings read from AVRCP responses\n");
    return 0;
}
