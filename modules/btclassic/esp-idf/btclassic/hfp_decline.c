// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Call audio declined, for chips whose controller can't carry it (the list
// is in hfp_decline.cmake). The hands-free unit still follows the phone's
// calls; their audio stays on the phone.
//
//   --wrap=btm_sco_conn_req   a phone asking for a SCO or eSCO link is turned
//       down at HCI, before Bluedroid or the controller start setting it up.
//   --wrap=PORT_WriteData     carries Android's audio policy command. Bluedroid
//       can't send an AT command of our own, so hfp_hf.c sends AT+XAPL with
//       a marker, and this swaps the marked command for
//       AT+ANDROID=SINKAUDIOPOLICY,2,2,2 on its way out. Its OK or ERROR
//       comes back as XAPL's, which Bluedroid expects and ignores.
//
// The policy (Android 14 and later) tells the phone not to move a call's
// audio to this unit and not to ring in band. Without it, an app that asks
// for the headset's audio makes the phone retry the declined link several
// times a second. Other phones answer ERROR, and the links they offer are
// declined. These are internal names, so the build turns this on only for
// the ESP-IDF versions it was written against.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "btc_bluedroid.h"
#include "hfp_decline.h"

#define HCI_LINK_TYPE_ACL 0x01
#define HCI_ERR_HOST_REJECT_RESOURCES 0x0d

void __real_btm_sco_conn_req(uint8_t *bda, uint8_t *dev_class, uint8_t link_type);
int __real_PORT_WriteData(uint16_t handle, char *p_data, uint16_t max_len, uint16_t *p_len);
uint8_t btsnd_hcic_reject_conn(uint8_t *bda, uint8_t reason);

void __wrap_btm_sco_conn_req(uint8_t *bda, uint8_t *dev_class, uint8_t link_type);
int __wrap_PORT_WriteData(uint16_t handle, char *p_data, uint16_t max_len, uint16_t *p_len);

void __wrap_btm_sco_conn_req(uint8_t *bda, uint8_t *dev_class, uint8_t link_type) {
    if (link_type != HCI_LINK_TYPE_ACL) {
        btsnd_hcic_reject_conn(bda, HCI_ERR_HOST_REJECT_RESOURCES);
        return;
    }
    __real_btm_sco_conn_req(bda, dev_class, link_type);
}

static const char policy[] = "AT+ANDROID=SINKAUDIOPOLICY,2,2,2\r";

int __wrap_PORT_WriteData(uint16_t handle, char *p_data, uint16_t max_len, uint16_t *p_len) {
    static const char marked[] = "AT+XAPL=" BTC_HF_POLICY_MARKER ",";
    if (p_data != NULL && max_len >= sizeof(marked) - 1 && memcmp(p_data, marked, sizeof(marked) - 1) == 0) {
        uint16_t n = 0;
        int rc = __real_PORT_WriteData(handle, (char *)policy, sizeof(policy) - 1, &n);
        if (p_len != NULL) {
            *p_len = n == sizeof(policy) - 1 ? max_len : n;
        }
        return rc;
    }
    return __real_PORT_WriteData(handle, p_data, max_len, p_len);
}
