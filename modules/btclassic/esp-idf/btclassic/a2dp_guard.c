// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Bluedroid in ESP-IDF 5.5 hands every A2DP and AVRCP link event to
// btc_a2dp_cb_handler, which passes it to the A2DP state machine without
// checking that the machine still exists. A link event that arrives after
// esp_a2d_sink_deinit() or esp_a2d_source_deinit() freed it (a sender's
// connection that was still opening, say) then reads freed memory and
// panics the board. ESP-IDF's newer handler drops such an event; this wraps
// the old one, at link time (--wrap=btc_a2dp_cb_handler), to do the same.

#include "btc/btc_task.h"
#include "bta/bta_av_api.h"
#include "esp_a2dp_api.h"
#include "osi/allocator.h"

void __real_btc_a2dp_cb_handler(btc_msg_t *msg);
void btc_a2dp_get_profile_status(esp_a2d_profile_status_t *param);

void __wrap_btc_a2dp_cb_handler(btc_msg_t *msg) {
    esp_a2d_profile_status_t st;
    btc_a2dp_get_profile_status(&st);
    if (st.a2d_snk_inited || st.a2d_src_inited) {
        __real_btc_a2dp_cb_handler(msg);
        return;
    }
    // Dropped: free what the handler would have.
    if (msg->act == BTA_AV_META_MSG_EVT) {
        tBTA_AV *av = (tBTA_AV *)msg->arg;
        if (av->meta_msg.p_data) {
            osi_free(av->meta_msg.p_data);
        }
        if (av->meta_msg.p_msg) {
            if (av->meta_msg.p_msg->vendor.p_vendor_data) {
                osi_free(av->meta_msg.p_msg->vendor.p_vendor_data);
            }
            osi_free(av->meta_msg.p_msg);
        }
    }
}
