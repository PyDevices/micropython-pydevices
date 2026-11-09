// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// A2DP and AVRCP, shared by the sink and the source. BTstack's deinit calls
// forget their lists without freeing what's in them or unregistering their
// L2CAP services, so the layers start once, while the radio is on, and the
// role that's started gets the AVRCP events. Only one A2DP role runs at a
// time, as on Bluedroid.

#include "btstack.h"

#include "btc_btstack.h"

static bool sink_up, source_up, avrcp_up;
static const btc_av_handlers_t *role;

static void on_avrcp(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (role && role->avrcp) {
        role->avrcp(packet_type, channel, packet, size);
    }
}

static void on_avrcp_ct(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (role && role->controller) {
        role->controller(packet_type, channel, packet, size);
    }
}

static void on_avrcp_tg(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (role && role->target) {
        role->target(packet_type, channel, packet, size);
    }
}

void btc_av_sink_init_once(void) {
    if (!sink_up) {
        a2dp_sink_init();
        sink_up = true;
    }
}

void btc_av_source_init_once(void) {
    if (!source_up) {
        a2dp_source_init();
        source_up = true;
    }
}

// GOEP, under cover art and the phone book: its init resets the one
// connection the phone book uses, so it runs once while the radio is on.
static bool goep_up;

void btc_goep_init_once(void) {
    if (!goep_up) {
        goep_client_init();
        goep_up = true;
    }
}

void btc_av_avrcp_use(const btc_av_handlers_t *handlers) {
    if (!avrcp_up) {
        avrcp_init();
        avrcp_controller_init();
        avrcp_target_init();
        btc_goep_init_once();
        avrcp_cover_art_client_init();
        avrcp_register_packet_handler(&on_avrcp);
        avrcp_controller_register_packet_handler(&on_avrcp_ct);
        avrcp_target_register_packet_handler(&on_avrcp_tg);
        avrcp_up = true;
    }
    role = handlers;
}

// The host went down (stopped, or a soft reset), taking the layers' memory
// with it: clear their state so the next start sets them up afresh.
void btc_av_radio_lost(void) {
    if (avrcp_up) {
        avrcp_cover_art_client_deinit();
        avrcp_target_deinit();
        avrcp_controller_deinit();
        avrcp_deinit();
    }
    if (goep_up) {
        goep_client_deinit();
        goep_up = false;
    }
    if (sink_up) {
        a2dp_sink_deinit();
    }
    if (source_up) {
        a2dp_source_deinit();
    }
    sink_up = source_up = avrcp_up = false;
    role = NULL;
}
