// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Inside btclassic's BTstack backend: what its files share, and the hooks
// the host program provides. btclassic.h is the backend's whole public face.
//
// Threads: BTstack runs every handler on its run loop. On MicroPython's rp2
// port that loop is a scheduled task on the interpreter's own thread, so the
// btc_* calls (also made from that thread) never race a handler, and nothing
// here takes a lock.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btclassic.h"
#include "btstack_defines.h"

// ---- hooks the host program provides ----

// Bring BTstack's HCI up and wait until it works (on MicroPython: the
// `bluetooth` module's own start, so BLE keeps working beside Classic), or
// take it down. 0 or a negative BTC_ERR_*.
int btc_btstack_host_start(void);
void btc_btstack_host_stop(void);
// Whether the stack is still up: a soft reset on MicroPython takes it down
// underneath the backend, whose state then has to be forgotten.
bool btc_btstack_host_active(void);

// Memory for the big buffers (audio rings, cover art), taken when a profile
// starts and given back when it stops, so a board pays only for what it
// opens. NULL when there isn't enough. A soft reset frees all of it under the
// backend, along with the stack (see btc_btstack_host_active).
void *btc_btstack_alloc(size_t size);
void btc_btstack_free(void *ptr);

// Persist a small blob (the link keys and the radio's settings) and read it
// back. load returns the bytes read, 0 for none.
void btc_btstack_store_save(const void *data, size_t len);
size_t btc_btstack_store_load(void *data, size_t size);

// ---- shared inside the backend ----

// A line in the backend's text log (a debugging aid; see btc_btstack_log_read).
void btc_log(const char *fmt, ...);
size_t btc_btstack_log_read(char *dst, size_t size);

// The radio tells the profiles when it goes down, and when an ACL link to a
// device goes (so a profile can clear what it knew of it).
void btc_sink_radio_stopping(void);
void btc_hf_radio_stopping(void);
void btc_spp_radio_stopping(void);
void btc_l2cap_radio_stopping(void);
// The stack went down underneath (a soft reset): forget, without calling it.
void btc_sink_radio_lost(void);
void btc_hf_radio_lost(void);
void btc_spp_radio_lost(void);
void btc_l2cap_radio_lost(void);
void btc_source_radio_stopping(void);
void btc_source_radio_lost(void);

// RFCOMM, shared by HFP and SPP: started once while the radio is on.
void btc_rfcomm_init_once(void);

// BTstack's SBC settings as btclassic.h reports them: the codec's bit
// fields, as in its capabilities, the way Bluedroid gives them.
static inline uint8_t btc_sbc_mode_bits(uint8_t avdtp_mode) {
    // AVDTP_CHANNEL_MODE_MONO 1, DUAL 2, STEREO 3, JOINT 4
    return avdtp_mode >= 1 && avdtp_mode <= 4 ? (uint8_t)(0x10 >> avdtp_mode) : 0;
}
static inline uint8_t btc_sbc_blocks_bits(uint8_t blocks) {
    return blocks == 4 ? 8 : blocks == 8 ? 4 : blocks == 12 ? 2 : blocks == 16 ? 1 : 0;
}
static inline uint8_t btc_sbc_subbands_bits(uint8_t subbands) {
    return subbands == 4 ? 2 : subbands == 8 ? 1 : 0;
}

void btc_hid_radio_stopping(void);
void btc_hid_radio_lost(void);
void btc_lib_radio_stopping(void);
void btc_lib_radio_lost(void);

// AVRCP browsing's events, which av.c routes to the library.
void btc_lib_on_browse(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
// The sink's AVRCP control link, which the library plays items over; 0 without one.
uint16_t btc_sink_avrcp_cid(void);
void btc_pb_radio_stopping(void);
void btc_pb_radio_lost(void);

// GOEP (OBEX's transport), shared by cover art and the phone book: started
// once while the radio is on, and cleared with the AV layers (av.c).
void btc_goep_init_once(void);

// While a role that needs it runs, the radio shows this Class of Device in
// place of its own (0 puts its own back).
// The profiles running, for the Class of Device: each says when it starts
// and stops, and the HID device gives its subclass.
#define BTC_PROFILE_SINK 0x01
#define BTC_PROFILE_SOURCE 0x02
#define BTC_PROFILE_HF 0x04
#define BTC_PROFILE_SERIAL 0x08
#define BTC_PROFILE_HID_DEVICE 0x10
void btc_radio_profile(uint32_t profile, bool running, uint8_t hid_subclass);

// A2DP and AVRCP layers, started once while the radio is on (av.c). The
// started role passes its AVRCP handlers, or NULL when it stops.
typedef struct {
    btstack_packet_handler_t avrcp;         // connections
    btstack_packet_handler_t controller;
    btstack_packet_handler_t target;
} btc_av_handlers_t;
void btc_av_sink_init_once(void);
void btc_av_source_init_once(void);
void btc_av_avrcp_use(const btc_av_handlers_t *handlers);
void btc_av_radio_lost(void);

// The last device a profile talked to, kept with the link keys so a sink
// can call it back after a reset.
void btc_radio_remember_peer(const uint8_t *addr);
bool btc_radio_last_peer(uint8_t *addr);
// And the last sink the source played to, for connect() with no address.
void btc_radio_remember_sink(const uint8_t *addr);
bool btc_radio_last_sink(uint8_t *addr);
