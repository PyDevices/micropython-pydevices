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
#define LAST_INPUTS 4           // input reports kept to answer GET_REPORT
#define FEATURES 4              // feature reports, by ID
#define GOT_MAX 128             // a GET_REPORT answer, ID first

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
    struct {
        uint8_t len;
        uint8_t report[MAX_REPORT];
    } last_input[LAST_INPUTS];
    struct {
        uint8_t len;
        uint8_t report[MAX_REPORT];
    } feature[FEATURES];
    // host: one GET_REPORT or SET_REPORT at a time
    int8_t got_state;       // 0 none, 1 waiting, 2 answered, -1 refused
    uint8_t got_len;
    uint8_t got[GOT_MAX];
    uint8_t set_buf[GOT_MAX];
    // host: the device's Device ID record
    bool have_ids;
    uint16_t ids[3];        // vendor, product, version
    uint16_t ids_cid;       // the link the query is for
} h;

// The last peer, for connect() with none; kept across stop and start.
static bool have_last;
static btc_addr_t last;

static uint8_t sdp_hid[400];
static uint8_t sdp_did[64];
static uint16_t dev_ids[3];     // device: the Device ID record's IDs, all 0 for none
static bool did_registered;
#define SDP_HID_OVERHEAD 160     // the record around the descriptor, name included
static uint8_t *descriptor;     // device: the program's, copied to the heap
static uint16_t descriptor_len;
static bool has_ids;            // device: the descriptor has report IDs
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
    if (h.got_state == 1) {
        h.got_state = -1;
    }
    h.have_ids = false;
    h.ids_cid = 0;
    h.changes |= BTC_CHANGED_CONNECTION;
    btc_log("hid: closed");
}

// ---- device ----

static bool ids_on_link(void) {
    return has_ids || (h.connected && hid_device_in_boot_protocol_mode(h.cid));
}

// Keep the last input report of each ID, to answer the host's GET_REPORT.
static void keep_input(const uint8_t *report, size_t n) {
    bool ids = ids_on_link();
    int slot = -1;
    for (int i = 0; i < LAST_INPUTS; i++) {
        if (h.last_input[i].len && (!ids || h.last_input[i].report[0] == report[0])) {
            slot = i;
            break;
        }
        if (slot < 0 && h.last_input[i].len == 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        h.last_input[slot].len = (uint8_t)n;
        memcpy(h.last_input[slot].report, report, n);
    }
}

// The feature report kept under an ID (any, without IDs), or with `make`
// a free slot for it; -1 for none.
static int feature_slot(uint8_t id, bool make) {
    int free_slot = -1;
    for (int i = 0; i < FEATURES; i++) {
        if (h.feature[i].len && (!has_ids || h.feature[i].report[0] == id)) {
            return i;
        }
        if (free_slot < 0 && h.feature[i].len == 0) {
            free_slot = i;
        }
    }
    return make ? free_slot : -1;
}

// The host asks for a report: the last input report with that ID, the
// last output report, or the feature report, without the ID (BTstack adds
// it). BTstack wants the
// size the descriptor (or the boot protocol) gives, so pad or cut to it.
static int on_get_report(uint16_t cid, hid_report_type_t type, uint16_t report_id, int *out_size, uint8_t *out) {
    bool ids = ids_on_link();
    const uint8_t *r = NULL;
    size_t n = 0;
    if (type == HID_REPORT_TYPE_INPUT) {
        for (int i = 0; i < LAST_INPUTS; i++) {
            if (h.last_input[i].len && (!ids || h.last_input[i].report[0] == report_id)) {
                r = h.last_input[i].report;
                n = h.last_input[i].len;
                break;
            }
        }
    } else if (type == HID_REPORT_TYPE_OUTPUT && h.output_len && (!ids || h.output[0] == report_id)) {
        r = h.output;
        n = h.output_len;
    } else if (type == HID_REPORT_TYPE_FEATURE) {
        int i = feature_slot((uint8_t)report_id, false);
        if (i >= 0) {
            r = h.feature[i].report;
            n = h.feature[i].len;
            if (!has_ids && ids) {
                ids = false;    // kept without an ID: nothing to strip
            }
        }
    }
    if (r && ids) {
        r++;
        n--;
    }
    int want;
    if (hid_device_in_boot_protocol_mode(cid)) {
        want = report_id == HID_BOOT_MODE_KEYBOARD_ID ? 8 : report_id == HID_BOOT_MODE_MOUSE_ID ? 3 : 0;
    } else {
        want = btstack_hid_get_report_size_for_id(report_id, type, descriptor_len, descriptor);
    }
    if (r == NULL || n == 0 || want <= 0 || want > MAX_REPORT) {
        *out_size = 0;      // refused: unsupported request
        return 1;
    }
    memset(out, 0, want);
    memcpy(out, r, n < (size_t)want ? n : (size_t)want);
    *out_size = want;
    return 1;
}

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
            if (n && hid_device_in_boot_protocol_mode(h.cid) && has_ids
                && msg[1] != HID_BOOT_MODE_KEYBOARD_ID && msg[1] != HID_BOOT_MODE_MOUSE_ID) {
                n = 0;      // the boot protocol has only the keyboard and the mouse
                h.dropped++;
            }
            if (n) {
                keep_input(msg + 1, n);
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
    if (type == HID_REPORT_TYPE_FEATURE && len > 0) {
        // Only a feature the program has given a value. BTstack has already
        // answered the host; a feature this device lacks is left alone.
        int i = feature_slot(has_ids ? report[0] : 0, false);
        if (i >= 0 && len <= MAX_REPORT) {
            memcpy(h.feature[i].report, report, len);
            h.feature[i].len = (uint8_t)len;
            h.changes |= BTC_CHANGED_HID;
        }
        return;
    }
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

// An output report on the interrupt channel. BTstack hands it over without
// its ID (SET_REPORT keeps it), so put the ID back.
static void on_report_data(uint16_t cid, hid_report_type_t type, uint16_t report_id, int len, uint8_t *report) {
    if (report_id == 0) {
        on_set_report(cid, type, len, report);
        return;
    }
    uint8_t r[MAX_REPORT];
    if (len > MAX_REPORT - 1) {
        len = MAX_REPORT - 1;
    }
    r[0] = (uint8_t)report_id;
    memcpy(r + 1, report, len);
    on_set_report(cid, type, len + 1, r);
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
    descriptor_len = (uint16_t)len;
    has_ids = btstack_hid_report_id_declared(descriptor_len, descriptor);
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
    if (dev_ids[0] || dev_ids[1] || dev_ids[2]) {
        memset(sdp_did, 0, sizeof(sdp_did));
        device_id_create_sdp_record(sdp_did, sdp_create_service_record_handle(),
            DEVICE_ID_VENDOR_ID_SOURCE_USB, dev_ids[0], dev_ids[1], dev_ids[2]);
        sdp_register_service(sdp_did);
        did_registered = true;
    }
    hid_device_init(boot, (uint16_t)len, descriptor);
    hid_device_register_packet_handler(&on_device);
    hid_device_register_set_report_callback(&on_set_report);
    hid_device_register_report_data_callback(&on_report_data);
    hid_device_register_report_request_callback(&on_get_report);
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

int btc_hid_device_set_feature(const uint8_t *report, size_t len) {
    if (!h.started || h.host) {
        return BTC_ERR_STATE;
    }
    if (len <= (has_ids ? 1u : 0u) || len > MAX_REPORT) {
        return BTC_ERR_ARG;
    }
    int i = feature_slot(has_ids ? report[0] : 0, true);
    if (i < 0) {
        return BTC_ERR_ARG;     // FEATURES IDs already
    }
    memcpy(h.feature[i].report, report, len);
    h.feature[i].len = (uint8_t)len;
    return BTC_OK;
}

size_t btc_hid_device_feature(uint8_t report_id, uint8_t *dst, size_t size) {
    int i = feature_slot(report_id, false);
    if (i < 0) {
        return 0;
    }
    size_t n = h.feature[i].len < size ? h.feature[i].len : size;
    memcpy(dst, h.feature[i].report, n);
    return n;
}

int btc_hid_device_set_ids(uint16_t vendor, uint16_t product, uint16_t version) {
    if (h.started && !h.host) {
        return BTC_ERR_STATE;   // the record goes in as the device starts
    }
    dev_ids[0] = vendor;
    dev_ids[1] = product;
    dev_ids[2] = version;
    return BTC_OK;
}

// ---- host ----

// The device's Device ID record, read over SDP once the link is up and the
// SDP client is free (the HID descriptor query uses it too).
static uint8_t ids_attr[8];
static btstack_context_callback_registration_t ids_reg;
static bool ids_reg_pending;

static void on_ids_query(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    bool ours = h.connected && h.host && h.cid == h.ids_cid;
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            uint16_t len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            uint16_t off = sdp_event_query_attribute_byte_get_data_offset(packet);
            if (len > sizeof(ids_attr) || off >= len) {
                break;
            }
            ids_attr[off] = sdp_event_query_attribute_byte_get_data(packet);
            if (off + 1 != len || !ours) {
                break;
            }
            uint16_t id = sdp_event_query_attribute_byte_get_attribute_id(packet);
            uint16_t v;
            if (id >= BLUETOOTH_ATTRIBUTE_VENDOR_ID && id <= BLUETOOTH_ATTRIBUTE_VERSION
                && de_element_get_uint16(ids_attr, &v)) {
                h.ids[id - BLUETOOTH_ATTRIBUTE_VENDOR_ID] = v;
                if (id == BLUETOOTH_ATTRIBUTE_VENDOR_ID) {
                    h.have_ids = v != 0xffff;
                }
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE:
            if (ours && h.have_ids) {
                h.changes |= BTC_CHANGED_HID;
                btc_log("hid: device id %04x:%04x v%04x", h.ids[0], h.ids[1], h.ids[2]);
            }
            break;
        default:
            break;
    }
}

static void ids_query_start(void *context) {
    (void)context;
    ids_reg_pending = false;
    if (!h.connected || !h.host || h.cid != h.ids_cid) {
        return;     // gone before the SDP client was free
    }
    sdp_client_query_uuid16(&on_ids_query, h.peer, BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION);
}

static void ids_query(void) {
    h.have_ids = false;
    h.ids_cid = h.cid;
    if (!ids_reg_pending) {
        ids_reg_pending = true;
        ids_reg.callback = &ids_query_start;
        sdp_client_register_query_callback(&ids_reg);
    }
}

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
            ids_query();
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            link_down();
            break;
        case HID_SUBEVENT_GET_REPORT_RESPONSE:
            if (h.got_state == 1) {
                uint16_t n = hid_subevent_get_report_response_get_report_len(packet);
                if (hid_subevent_get_report_response_get_handshake_status(packet) == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL && n) {
                    if (n > GOT_MAX) {
                        n = GOT_MAX;
                    }
                    memcpy(h.got, hid_subevent_get_report_response_get_report(packet), n);
                    h.got_len = (uint8_t)n;
                    h.got_state = 2;
                } else {
                    h.got_state = -1;
                }
                h.changes |= BTC_CHANGED_HID;
            }
            break;
        case HID_SUBEVENT_SET_REPORT_RESPONSE:
            if (h.got_state == 1) {
                h.got_len = 0;
                h.got_state = hid_subevent_set_report_response_get_handshake_status(packet) == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL ? 2 : -1;
                h.changes |= BTC_CHANGED_HID;
            }
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

bool btc_hid_host_ids(uint16_t *vendor, uint16_t *product, uint16_t *version) {
    if (!h.connected || !h.host || !h.have_ids) {
        return false;
    }
    *vendor = h.ids[0];
    *product = h.ids[1];
    *version = h.ids[2];
    return true;
}

static int request_result(uint8_t s) {
    if (s == ERROR_CODE_SUCCESS) {
        return BTC_OK;
    }
    h.got_state = 0;
    return s == ERROR_CODE_COMMAND_DISALLOWED ? BTC_ERR_STATE : BTC_ERR_FAIL;
}

int btc_hid_host_get_report(uint8_t type, uint8_t report_id) {
    if (!h.connected || !h.host) {
        return BTC_ERR_STATE;
    }
    if (type < BTC_HID_INPUT || type > BTC_HID_FEATURE) {
        return BTC_ERR_ARG;
    }
    if (h.got_state == 1) {
        return BTC_ERR_STATE;
    }
    h.got_state = 1;
    h.got_len = 0;
    return request_result(hid_host_send_get_report(h.cid, (hid_report_type_t)type, report_id));
}

int btc_hid_host_got_report(uint8_t *dst, size_t size) {
    int state = h.got_state;
    if (state == 1) {
        return h.connected ? 0 : BTC_ERR_STATE;
    }
    h.got_state = 0;
    if (state < 0) {
        return BTC_ERR_FAIL;
    }
    if (state != 2) {
        return BTC_ERR_STATE;
    }
    size_t n = h.got_len < size ? h.got_len : size;
    memcpy(dst, h.got, n);
    return (int)n;
}

int btc_hid_host_set_report(uint8_t type, const uint8_t *report, size_t len) {
    if (!h.connected || !h.host) {
        return BTC_ERR_STATE;
    }
    if (type < BTC_HID_INPUT || type > BTC_HID_FEATURE || len == 0 || len > GOT_MAX) {
        return BTC_ERR_ARG;
    }
    if (h.got_state == 1) {
        return BTC_ERR_STATE;
    }
    h.got_state = 1;
    h.got_len = 0;
    // BTstack sends a byte before the rest as the report ID: the report's
    // first byte, which is its ID when it has one, so it goes out as given.
    // It sends from this buffer later, so the copy has to outlive the call.
    memcpy(h.set_buf, report, len);
    return request_result(hid_host_send_set_report(h.cid, (hid_report_type_t)type, h.set_buf[0], h.set_buf + 1, (uint8_t)(len - 1)));
}

int btc_hid_host_set_done(void) {
    int state = h.got_state;
    if (state == 1) {
        return h.connected ? 0 : BTC_ERR_STATE;
    }
    h.got_state = 0;
    return state == 2 ? 1 : BTC_ERR_FAIL;
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
        if (did_registered) {
            sdp_unregister_service(big_endian_read_32(sdp_did, 9));
            did_registered = false;
        }
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
    did_registered = false;
    ids_reg_pending = false;
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
