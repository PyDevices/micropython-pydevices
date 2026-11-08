// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// HID over Bluetooth Classic on BTstack, one role at a time: the board as a
// device (a keyboard, mouse or gamepad a PC or phone pairs with), or as a
// host (it pairs with a Bluetooth keyboard or gamepad and reads its
// reports). Both use the same two L2CAP channels, so a role takes them when
// it starts and gives them back when it stops.
//
// Reports go in and out as the descriptor lays them out, report ID first
// when the descriptor has IDs; this file adds and strips the HID transaction
// header byte (0xa1 DATA|Input, 0xa2 DATA|Output).

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

#define QUEUE_BYTES 512         // reports waiting, each with a length byte
#define MAX_REPORT 64
#define HOST_DESCRIPTOR_MAX 512

#define MINOR_KEYBOARD 0x40       // the minor device class: 0x40 keyboard, 0x80 mouse, 0x08 gamepad

typedef struct {
    uint8_t buf[QUEUE_BYTES];
    uint16_t head, count;
} queue_t;

static struct {
    bool started;
    bool host;              // the role
    bool connected;
    bool boot;              // the link runs the boot protocol
    uint16_t cid;
    btc_addr_t peer;
    uint32_t sent, received, dropped;
    uint32_t changes;
    queue_t q;              // device: reports to send; host: reports received
    uint8_t output[MAX_REPORT + 1];     // device: the last report the host set
    uint8_t output_len;
} h;

// The last peer, for connect() with none; kept across stop and start.
static bool have_last;
static btc_addr_t last;

static uint8_t sdp_hid[400];
#define SDP_HID_OVERHEAD 160     // the record around the descriptor, name included
static uint8_t *descriptor;     // device: the program's, copied to the heap
static uint8_t host_descriptors[HOST_DESCRIPTOR_MAX];

// ---- the queue: length-prefixed reports ----

static bool queue_put(const uint8_t *data, size_t len) {
    if (len == 0 || len > MAX_REPORT || h.q.count + len + 1 > QUEUE_BYTES) {
        return false;
    }
    uint16_t at = (h.q.head + h.q.count) % QUEUE_BYTES;
    h.q.buf[at] = (uint8_t)len;
    for (size_t i = 0; i < len; i++) {
        h.q.buf[(at + 1 + i) % QUEUE_BYTES] = data[i];
    }
    h.q.count += len + 1;
    return true;
}

static size_t queue_get(uint8_t *dst, size_t size) {
    if (h.q.count == 0) {
        return 0;
    }
    size_t len = h.q.buf[h.q.head];
    for (size_t i = 0; i < len; i++) {
        uint8_t b = h.q.buf[(h.q.head + 1 + i) % QUEUE_BYTES];
        if (i < size) {
            dst[i] = b;
        }
    }
    h.q.head = (h.q.head + len + 1) % QUEUE_BYTES;
    h.q.count -= len + 1;
    return len < size ? len : size;
}

static void link_up(const uint8_t *packet) {
    h.cid = hid_subevent_connection_opened_get_hid_cid(packet);
    hid_subevent_connection_opened_get_bd_addr(packet, h.peer);
    memcpy(last, h.peer, 6);
    have_last = true;
    h.connected = true;
    h.changes |= BTC_CHANGED_CONNECTION;
    btc_log("hid: %s connected", bd_addr_to_str(h.peer));
}

static void link_down(void) {
    h.cid = 0;
    h.connected = false;
    h.boot = false;
    h.q.head = h.q.count = 0;
    h.changes |= BTC_CHANGED_CONNECTION;
    btc_log("hid: closed");
}

// ---- device ----

static void on_device(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) {
        return;
    }
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_CONNECTION_OPENED:
            if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                btc_log("hid: connect failed 0x%02x", hid_subevent_connection_opened_get_status(packet));
                h.cid = 0;
                break;
            }
            link_up(packet);
            if (h.q.count) {
                hid_device_request_can_send_now_event(h.cid);
            }
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            link_down();
            break;
        case HID_SUBEVENT_CAN_SEND_NOW: {
            uint8_t msg[MAX_REPORT + 1];
            size_t n = queue_get(msg + 1, MAX_REPORT);
            if (n) {
                msg[0] = 0xa1;     // DATA, Input
                hid_device_send_interrupt_message(h.cid, msg, (uint16_t)(n + 1));
                h.sent++;
            }
            if (h.q.count) {
                hid_device_request_can_send_now_event(h.cid);
            } else {
                h.changes |= BTC_CHANGED_HID;  // room again
            }
            break;
        }
        case HID_SUBEVENT_SUSPEND:
        case HID_SUBEVENT_EXIT_SUSPEND:
            h.changes |= BTC_CHANGED_HID;
            break;
        default:
            break;
    }
}

// The host sets an output report: a keyboard's LEDs.
static void on_set_report(uint16_t cid, hid_report_type_t type, int len, uint8_t *report) {
    (void)cid;
    if (type != HID_REPORT_TYPE_OUTPUT || len <= 0) {
        return;
    }
    if (len > MAX_REPORT) {
        len = MAX_REPORT;
    }
    memcpy(h.output, report, len);
    h.output_len = (uint8_t)len;
    h.changes |= BTC_CHANGED_HID;
}

static void on_report_data(uint16_t cid, hid_report_type_t type, uint16_t report_id, int len, uint8_t *report) {
    (void)report_id;
    on_set_report(cid, type, len, report);
}

static void ignore(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)packet;
    (void)size;
}

int btc_hid_device_start(const uint8_t *desc, size_t len, uint8_t subclass, const char *name, bool boot) {
    if (h.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    size_t name_len = name ? strlen(name) : 0;
    if (desc == NULL || len == 0 || len + name_len + SDP_HID_OVERHEAD > sizeof(sdp_hid)) {
        return BTC_ERR_ARG;     // too big for the service record
    }
    descriptor = btc_btstack_alloc(len);
    if (descriptor == NULL) {
        return BTC_ERR_NOMEM;
    }
    memcpy(descriptor, desc, len);
    memset(&h, 0, sizeof(h));

    hid_sdp_record_t params = {
        subclass ? subclass : MINOR_KEYBOARD, 33,   // US
        0, 1, 1, 1,         // virtual cable, remote wake, reconnect initiate, normally connectable
        boot,
        1600, 3200, 3200,   // sniff subrating latency and timeout, supervision timeout
        descriptor, (uint16_t)len,
        name && *name ? name : "Keyboard",
    };
    memset(sdp_hid, 0, sizeof(sdp_hid));
    hid_create_sdp_record(sdp_hid, sdp_create_service_record_handle(), &params);
    sdp_register_service(sdp_hid);
    hid_device_init(boot, (uint16_t)len, descriptor);
    hid_device_register_packet_handler(&on_device);
    hid_device_register_set_report_callback(&on_set_report);
    hid_device_register_report_data_callback(&on_report_data);
    // Peripheral, and what kind, unless audio runs too: see radio.c.
    btc_radio_profile(BTC_PROFILE_HID_DEVICE, true, subclass ? subclass : MINOR_KEYBOARD);
    h.started = true;
    h.host = false;
    return BTC_OK;
}

int btc_hid_device_connect(const uint8_t *addr) {
    if (!h.started || h.host) {
        return BTC_ERR_STATE;
    }
    if (h.connected) {
        return BTC_OK;
    }
    bd_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else if (have_last) {
        memcpy(a, last, 6);
    } else if (!btc_radio_last_peer(a)) {
        return BTC_ERR_ARG;
    }
    uint8_t s = hid_device_connect(a, &h.cid);
    btc_log("hid: calling %s: 0x%02x", bd_addr_to_str(a), s);
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

int btc_hid_device_send(const uint8_t *report, size_t len) {
    if (!h.started || h.host) {
        return BTC_ERR_STATE;
    }
    if (len == 0 || len > MAX_REPORT) {
        return BTC_ERR_ARG;
    }
    if (!h.connected) {
        return 0;   // no host: drop it, or it would be typed on reconnect
    }
    if (!queue_put(report, len)) {
        return 0;   // full: try again after the queue drains
    }
    hid_device_request_can_send_now_event(h.cid);
    return 1;
}

size_t btc_hid_device_output(uint8_t *dst, size_t size) {
    size_t n = h.output_len < size ? h.output_len : size;
    memcpy(dst, h.output, n);
    return n;
}

// ---- host ----

static void on_host(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) {
        return;
    }
    switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            // A paired keyboard or gamepad calling back.
            if (h.connected) {
                hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
            } else {
                hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet),
                    HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            }
            break;
        case HID_SUBEVENT_CONNECTION_OPENED:
            if (hid_subevent_connection_opened_get_status(packet) != ERROR_CODE_SUCCESS) {
                btc_log("hid: connect failed 0x%02x", hid_subevent_connection_opened_get_status(packet));
                h.cid = 0;
                break;
            }
            link_up(packet);
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            link_down();
            break;
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            h.changes |= BTC_CHANGED_HID;
            break;
        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            h.boot = hid_subevent_set_protocol_response_get_protocol_mode(packet) == HID_PROTOCOL_MODE_BOOT;
            break;
        case HID_SUBEVENT_REPORT: {
            const uint8_t *r = hid_subevent_report_get_report(packet);
            uint16_t len = hid_subevent_report_get_report_len(packet);
            if (len < 2 || r[0] != 0xa1) {
                break;  // only input reports
            }
            h.received++;
            if (!queue_put(r + 1, len - 1)) {
                h.dropped++;
            }
            h.changes |= BTC_CHANGED_HID;
            break;
        }
        default:
            break;
    }
}

int btc_hid_host_start(void) {
    if (h.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    memset(&h, 0, sizeof(h));
    hid_host_init(host_descriptors, sizeof(host_descriptors));
    hid_host_register_packet_handler(&on_host);
    h.started = true;
    h.host = true;
    return BTC_OK;
}

int btc_hid_host_connect(const uint8_t *addr, bool boot) {
    if (!h.started || !h.host) {
        return BTC_ERR_STATE;
    }
    if (h.connected) {
        return BTC_OK;
    }
    bd_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else if (have_last) {
        memcpy(a, last, 6);
    } else {
        return BTC_ERR_ARG;
    }
    uint8_t s = hid_host_connect(a, boot ? HID_PROTOCOL_MODE_BOOT : HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT, &h.cid);
    btc_log("hid: calling %s: 0x%02x", bd_addr_to_str(a), s);
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

size_t btc_hid_host_read(uint8_t *dst, size_t size) {
    if (!h.started || !h.host) {
        return 0;
    }
    return queue_get(dst, size);
}

int btc_hid_host_send(uint8_t report_id, const uint8_t *data, size_t len) {
    if (!h.connected || !h.host || len > MAX_REPORT) {
        return BTC_ERR_STATE;
    }
    uint8_t s = hid_host_send_report(h.cid, report_id, data, (uint8_t)len);
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

size_t btc_hid_host_descriptor(uint8_t *dst, size_t size) {
    if (!h.connected || !h.host) {
        return 0;
    }
    size_t n = hid_descriptor_storage_get_descriptor_len(h.cid);
    const uint8_t *d = hid_descriptor_storage_get_descriptor_data(h.cid);
    if (d == NULL) {
        return 0;
    }
    if (n > size) {
        n = size;
    }
    memcpy(dst, d, n);
    return n;
}

// ---- both ----

int btc_hid_disconnect(void) {
    if (!h.started) {
        return BTC_ERR_STATE;
    }
    if (h.cid) {
        if (h.host) {
            hid_host_disconnect(h.cid);
        } else {
            hid_device_disconnect(h.cid);
        }
    }
    return BTC_OK;
}

void btc_hid_stop(void) {
    if (!h.started) {
        return;
    }
    btc_hid_disconnect();
    // Give the channels back: the other role, or the same one again,
    // registers them afresh. Open channels close on their own.
    l2cap_unregister_service(PSM_HID_INTERRUPT);
    l2cap_unregister_service(PSM_HID_CONTROL);
    if (h.host) {
        hid_host_register_packet_handler(&ignore);
    } else {
        hid_device_register_packet_handler(&ignore);
        sdp_unregister_service(big_endian_read_32(sdp_hid, 9));
        btc_radio_profile(BTC_PROFILE_HID_DEVICE, false, 0);
        btc_btstack_free(descriptor);
        descriptor = NULL;
    }
    h.started = false;
    h.connected = false;
    h.cid = 0;
}

void btc_hid_radio_stopping(void) {
    btc_hid_stop();
}

void btc_hid_radio_lost(void) {
    memset(&h, 0, sizeof(h));
    descriptor = NULL;
}

void btc_hid_status(btc_hid_status_t *out) {
    out->started = h.started;
    out->host = h.host;
    out->connected = h.connected;
    memcpy(out->peer, h.peer, 6);
    out->boot = h.host ? h.boot : (h.connected && hid_device_in_boot_protocol_mode(h.cid));
    out->sent = h.sent;
    out->received = h.received;
    out->dropped = h.dropped;
    out->queued = h.q.count;
}

uint32_t btc_hid_changes(void) {
    uint32_t c = h.changes;
    h.changes = 0;
    return c;
}
