// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's radio on BTstack: GAP (name, visibility, SSP pairing), the
// paired devices and their link keys (kept through the host's store hook),
// SDP, and inquiry.

#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

// ---- the log ----

#define LOG_SIZE 2048
static char log_buf[LOG_SIZE];
static size_t log_head, log_len;

void btc_log(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof(line) - 2) {
        n = sizeof(line) - 2;
    }
    line[n++] = '\n';
    for (int i = 0; i < n; i++) {
        log_buf[(log_head + log_len) % LOG_SIZE] = line[i];
        if (log_len < LOG_SIZE) {
            log_len++;
        } else {
            log_head = (log_head + 1) % LOG_SIZE;
        }
    }
}

size_t btc_btstack_log_read(char *dst, size_t size) {
    size_t n = 0;
    while (n < size && log_len) {
        dst[n++] = log_buf[log_head];
        log_head = (log_head + 1) % LOG_SIZE;
        log_len--;
    }
    return n;
}

// ---- the store: link keys and the last peer ----

#define MAX_KEYS 8
#define STORE_MAGIC 0x31435442u     // "BTC1"

typedef struct {
    uint8_t addr[6];
    uint8_t type;       // link_key_type_t; 0 = empty slot
    uint8_t key[16];
} key_entry_t;

typedef struct {
    uint32_t magic;
    uint32_t serial;    // bumped on each use, so the oldest key goes first
    key_entry_t keys[MAX_KEYS];
    uint32_t used[MAX_KEYS];
    uint8_t last_peer[6];
    uint8_t has_last_peer;
    uint8_t pad;
    // Added later: a store without these loads with them empty.
    uint8_t last_sink[6];   // the last sink the source played to
    uint8_t has_last_sink;
    uint8_t pad2;
} store_t;

static store_t store;

static void store_save(void) {
    btc_btstack_store_save(&store, sizeof(store));
}

static void store_load(void) {
    memset(&store, 0, sizeof(store));
    size_t n = btc_btstack_store_load(&store, sizeof(store));
    if (n < offsetof(store_t, last_sink) || store.magic != STORE_MAGIC) {
        memset(&store, 0, sizeof(store));
        store.magic = STORE_MAGIC;
    }
}

static int key_find(const uint8_t *addr) {
    for (int i = 0; i < MAX_KEYS; i++) {
        if (store.keys[i].type && memcmp(store.keys[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t radio_changed;

static void db_open(void) {
}

static void db_set_local_bd_addr(bd_addr_t addr) {
    (void)addr;
}

static void db_close(void) {
}

static int db_get_link_key(bd_addr_t addr, link_key_t key, link_key_type_t *type) {
    int i = key_find(addr);
    if (i < 0) {
        return 0;
    }
    memcpy(key, store.keys[i].key, 16);
    *type = (link_key_type_t)store.keys[i].type;
    return 1;
}

static void db_put_link_key(bd_addr_t addr, link_key_t key, link_key_type_t type) {
    int i = key_find(addr);
    if (i < 0) {
        // A free slot, or the least recently used one.
        uint32_t oldest = UINT32_MAX;
        for (int j = 0; j < MAX_KEYS; j++) {
            if (!store.keys[j].type) {
                i = j;
                break;
            }
            if (store.used[j] < oldest) {
                oldest = store.used[j];
                i = j;
            }
        }
    }
    memcpy(store.keys[i].addr, addr, 6);
    memcpy(store.keys[i].key, key, 16);
    store.keys[i].type = (uint8_t)(type ? type : 0xff);
    store.used[i] = ++store.serial;
    store_save();
    radio_changed |= BTC_CHANGED_PAIRED;
    btc_log("link key stored for %s type %u", bd_addr_to_str(addr), type);
}

static void db_delete_link_key(bd_addr_t addr) {
    int i = key_find(addr);
    if (i >= 0) {
        memset(&store.keys[i], 0, sizeof(store.keys[i]));
        store.used[i] = 0;
        store_save();
        radio_changed |= BTC_CHANGED_PAIRED;
        btc_log("link key deleted for %s", bd_addr_to_str(addr));
    }
}

static int db_iterator_init(btstack_link_key_iterator_t *it) {
    it->context = (void *)0;
    return 1;
}

static int db_iterator_get_next(btstack_link_key_iterator_t *it, bd_addr_t addr, link_key_t key, link_key_type_t *type) {
    for (uintptr_t i = (uintptr_t)it->context; i < MAX_KEYS; i++) {
        if (store.keys[i].type) {
            memcpy(addr, store.keys[i].addr, 6);
            memcpy(key, store.keys[i].key, 16);
            *type = (link_key_type_t)store.keys[i].type;
            it->context = (void *)(i + 1);
            return 1;
        }
    }
    it->context = (void *)MAX_KEYS;
    return 0;
}

static void db_iterator_done(btstack_link_key_iterator_t *it) {
    (void)it;
}

static const btstack_link_key_db_t key_db = {
    db_open, db_set_local_bd_addr, db_close,
    db_get_link_key, db_put_link_key, db_delete_link_key,
    db_iterator_init, db_iterator_get_next, db_iterator_done,
};

void btc_radio_remember_peer(const uint8_t *addr) {
    if (store.has_last_peer && memcmp(store.last_peer, addr, 6) == 0) {
        return;
    }
    memcpy(store.last_peer, addr, 6);
    store.has_last_peer = 1;
    store_save();
}

void btc_radio_remember_sink(const uint8_t *addr) {
    if (store.has_last_sink && memcmp(store.last_sink, addr, 6) == 0) {
        return;
    }
    memcpy(store.last_sink, addr, 6);
    store.has_last_sink = 1;
    store_save();
}

bool btc_radio_last_sink(uint8_t *addr) {
    if (!store.has_last_sink) {
        return false;
    }
    memcpy(addr, store.last_sink, 6);
    return true;
}

bool btc_radio_last_peer(uint8_t *addr) {
    if (!store.has_last_peer || key_find(store.last_peer) < 0) {
        return false;
    }
    memcpy(addr, store.last_peer, 6);
    return true;
}

// ---- the radio ----

static bool active;
static bool rfcomm_up;

// ---- Class of Device ----
//
// Worked out from every profile running, so a speaker that also runs a
// keyboard stays Audio/Video, and set again each time one starts or stops.
// config(device_class=...) replaces it whatever runs.

#define COD_SRVC_RENDERING (1u << 18)
#define COD_SRVC_CAPTURING (1u << 19)
#define COD_SRVC_AUDIO (1u << 21)
#define COD_SRVC_TELEPHONY (1u << 22)
#define COD_MAJOR_PERIPHERAL 0x05
#define COD_MAJOR_AV 0x04
#define COD_MAJOR_UNCATEGORIZED 0x1f

static uint32_t profiles;
static uint8_t hid_subclass;
static uint32_t class_set;      // config(device_class=...), or 0

static uint32_t auto_class(void) {
    uint32_t service = 0, major, minor;
    if (profiles & BTC_PROFILE_SINK) {
        service |= COD_SRVC_AUDIO | COD_SRVC_RENDERING;
    }
    if (profiles & BTC_PROFILE_SOURCE) {
        service |= COD_SRVC_AUDIO | COD_SRVC_CAPTURING;
    }
    if (profiles & BTC_PROFILE_HF) {
        service |= COD_SRVC_AUDIO | COD_SRVC_TELEPHONY;
    }
    if (profiles & (BTC_PROFILE_SINK | BTC_PROFILE_SOURCE | BTC_PROFILE_HF)) {
        // Audio/Video, even with a keyboard: a computer offers audio by it.
        major = COD_MAJOR_AV;
        minor = (profiles & BTC_PROFILE_HF) ? 0x02     // hands-free
            : (profiles & BTC_PROFILE_SINK) ? 0x05     // loudspeaker
            : 0x00;                                    // unclassified
    } else if (profiles & BTC_PROFILE_HID_DEVICE) {
        major = COD_MAJOR_PERIPHERAL;
        minor = hid_subclass >> 2;
    } else {
        major = COD_MAJOR_UNCATEGORIZED;
        minor = 0;
    }
    return service | major << 8 | minor << 2;
}

uint32_t btc_radio_class(void) {
    return class_set ? class_set : auto_class();
}

static void apply_class(void) {
    if (active) {
        gap_set_class_of_device(btc_radio_class());
    }
}

void btc_radio_profile(uint32_t profile, bool running, uint8_t subclass) {
    if (running) {
        profiles |= profile;
        if (profile & BTC_PROFILE_HID_DEVICE) {
            hid_subclass = subclass;
        }
    } else {
        profiles &= ~profile;
    }
    apply_class();
}

int btc_radio_set_class(uint32_t cod) {
    if (cod > 0xffffff || (cod & 3)) {
        return BTC_ERR_ARG;
    }
    class_set = cod;
    apply_class();
    return BTC_OK;
}

void btc_rfcomm_init_once(void) {
    if (!rfcomm_up) {
        rfcomm_init();
        rfcomm_up = true;
    }
}
static bool host_started;
static char radio_name[249] = "PicoW-BT";
static char radio_pin[17] = "0000";
static bool discoverable = true, connectable = true;
static btstack_packet_callback_registration_t hci_cb;

// Inquiry: what it found, and for each the paging details a remote name
// request needs when the device sent no name of its own.
static btc_found_t found[BTC_FOUND_MAX];
static struct {
    uint8_t psrm;
    uint16_t clock_offset;
    bool asked;
} found_page[BTC_FOUND_MAX];
static int found_count;
static bool inquiring;          // the inquiry, then its name requests
static bool scanning;           // the inquiry itself
static int naming = -1;         // the entry a name request is out for

static void found_add(const uint8_t *packet) {
    bd_addr_t addr;
    gap_event_inquiry_result_get_bd_addr(packet, addr);
    int i;
    for (i = 0; i < found_count; i++) {
        if (memcmp(found[i].addr, addr, 6) == 0) {
            break;
        }
    }
    if (i == found_count) {
        if (found_count == BTC_FOUND_MAX) {
            return;
        }
        memset(&found[i], 0, sizeof(found[i]));
        memset(&found_page[i], 0, sizeof(found_page[i]));
        memcpy(found[i].addr, addr, 6);
        found[i].rssi = BTC_UNKNOWN;
        found_count++;
    }
    btc_found_t *f = &found[i];
    f->cod = gap_event_inquiry_result_get_class_of_device(packet);
    if (gap_event_inquiry_result_get_rssi_available(packet)) {
        f->rssi = (int8_t)gap_event_inquiry_result_get_rssi(packet);
    }
    if (gap_event_inquiry_result_get_name_available(packet)) {
        size_t n = gap_event_inquiry_result_get_name_len(packet);
        if (n > sizeof(f->name) - 1) {
            n = sizeof(f->name) - 1;
        }
        memcpy(f->name, gap_event_inquiry_result_get_name(packet), n);
        f->name[n] = 0;
    }
    found_page[i].psrm = gap_event_inquiry_result_get_page_scan_repetition_mode(packet);
    found_page[i].clock_offset = gap_event_inquiry_result_get_clock_offset(packet);
    radio_changed |= BTC_CHANGED_FOUND;
}

// After the inquiry, ask each device that sent no name for it, one at a time.
static void found_next_name(void) {
    naming = -1;
    if (inquiring && !scanning) {
        for (int i = 0; i < found_count; i++) {
            if (!found[i].name[0] && !found_page[i].asked) {
                found_page[i].asked = true;
                if (gap_remote_name_request(found[i].addr, found_page[i].psrm,
                    found_page[i].clock_offset | 0x8000) == ERROR_CODE_SUCCESS) {
                    naming = i;
                    return;
                }
            }
        }
    }
    if (inquiring) {
        inquiring = false;
        radio_changed |= BTC_CHANGED_FOUND;
        btc_log("inquiry done: %d found", found_count);
    }
}

static void found_name(const uint8_t *packet) {
    if (naming < 0) {
        return;
    }
    bd_addr_t addr;
    hci_event_remote_name_request_complete_get_bd_addr(packet, addr);
    if (memcmp(addr, found[naming].addr, 6) != 0) {
        return;     // a profile's own request
    }
    if (hci_event_remote_name_request_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
        const char *name = (const char *)hci_event_remote_name_request_complete_get_remote_name(packet);
        btc_found_t *f = &found[naming];
        size_t n = strnlen(name, 248);
        if (n > sizeof(f->name) - 1) {
            n = sizeof(f->name) - 1;
        }
        memcpy(f->name, name, n);
        f->name[n] = 0;
        radio_changed |= BTC_CHANGED_FOUND;
    }
    found_next_name();
}

// The controller answers inquiries without RSSI until told otherwise, and
// BTstack only says so while it powers up, which the `bluetooth` module may
// have done already. So it's sent once the radio is on, when HCI is free.
static bool inquiry_mode_pending;

static void send_inquiry_mode(void) {
    if (inquiry_mode_pending && hci_can_send_command_packet_now()) {
        inquiry_mode_pending = false;
        hci_send_cmd(&hci_write_inquiry_mode, (int)INQUIRY_MODE_RSSI_AND_EIR);
    }
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    send_inquiry_mode();
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_CONNECTION_REQUEST:
            hci_event_connection_request_get_bd_addr(packet, addr);
            btc_log("acl request from %s cod %06x", bd_addr_to_str(addr),
                (unsigned)hci_event_connection_request_get_class_of_device(packet));
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            hci_event_connection_complete_get_bd_addr(packet, addr);
            btc_log("acl %s status %u handle %u", bd_addr_to_str(addr),
                hci_event_connection_complete_get_status(packet),
                hci_event_connection_complete_get_connection_handle(packet));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            btc_log("disconnected handle %u reason 0x%02x",
                hci_event_disconnection_complete_get_connection_handle(packet),
                hci_event_disconnection_complete_get_reason(packet));
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, addr);
            btc_log("legacy pairing with %s: pin", bd_addr_to_str(addr));
            gap_pin_code_response(addr, radio_pin);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            btc_log("ssp confirm %06u, accepted",
                (unsigned)hci_event_user_confirmation_request_get_numeric_value(packet));
            break;
        case GAP_EVENT_PAIRING_COMPLETE:
            gap_event_pairing_complete_get_bd_addr(packet, addr);
            btc_log("pairing with %s status 0x%02x", bd_addr_to_str(addr),
                gap_event_pairing_complete_get_status(packet));
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            btc_log("encryption %u on handle %u",
                hci_event_encryption_change_get_encryption_enabled(packet),
                hci_event_encryption_change_get_connection_handle(packet));
            break;
        case GAP_EVENT_INQUIRY_RESULT:
            found_add(packet);
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            scanning = false;
            found_next_name();
            break;
        case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
            found_name(packet);
            break;
        default:
            break;
    }
}

// A soft reset takes BTstack down without telling the backend. Notice it
// and forget what was started, so the next start begins again.
static void sync_host(void) {
    if (active && !btc_btstack_host_active()) {
        btc_spp_radio_lost();
        btc_hf_radio_lost();
        btc_sink_radio_lost();
        btc_source_radio_lost();
        btc_hid_radio_lost();
        btc_av_radio_lost();
        active = false;
        host_started = false;
        rfcomm_up = false;
        inquiring = scanning = false;
        naming = -1;
        profiles = 0;
    }
}

#ifdef ENABLE_HFP_DECLINE_SCO
// Takes ACL links only: SCO and eSCO requests are declined, so a call's audio
// stays on the phone and the link to it holds.
static int acl_only(bd_addr_t addr, hci_link_type_t link_type) {
    (void)addr;
    return link_type == HCI_LINK_TYPE_ACL;
}
#endif

int btc_radio_start(void) {
    sync_host();
    if (active) {
        return BTC_OK;
    }
    int err = btc_btstack_host_start();
    if (err) {
        return err;
    }
    host_started = true;
    store_load();

    hci_set_link_key_db(&key_db);
    hci_cb.callback = &hci_handler;
    hci_add_event_handler(&hci_cb);
    #ifdef ENABLE_HFP_DECLINE_SCO
    gap_register_classic_connection_filter(acl_only);
    #endif

    gap_set_local_name(radio_name);
    gap_set_class_of_device(btc_radio_class());
    // Answers with RSSI and the extended reply (names come with it).
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);
    inquiry_mode_pending = true;
    send_inquiry_mode();
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_auto_accept(1);
    gap_set_bondable_mode(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

    sdp_init();

    active = true;
    gap_connectable_control(connectable);
    gap_discoverable_control(discoverable);
    bd_addr_t addr;
    gap_local_bd_addr(addr);
    btc_log("classic up as %s, %s", radio_name, bd_addr_to_str(addr));
    return BTC_OK;
}

void btc_radio_stop(void) {
    sync_host();
    if (!active) {
        return;
    }
    btc_spp_radio_stopping();
    btc_hf_radio_stopping();
    btc_sink_radio_stopping();
    btc_source_radio_stopping();
    btc_hid_radio_stopping();
    gap_discoverable_control(0);
    gap_connectable_control(0);
    hci_remove_event_handler(&hci_cb);
    sdp_deinit();
    if (rfcomm_up) {
        rfcomm_deinit();
        rfcomm_up = false;
    }
    active = false;
    profiles = 0;
    if (host_started) {
        btc_btstack_host_stop();
        host_started = false;
        btc_av_radio_lost();
    }
}

bool btc_radio_active(void) {
    sync_host();
    return active;
}

int btc_radio_set_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 248) {
        return BTC_ERR_ARG;
    }
    memcpy(radio_name, name, n + 1);
    if (active) {
        gap_set_local_name(radio_name);
    }
    return BTC_OK;
}

size_t btc_radio_name(char *dst, size_t size) {
    size_t n = strlen(radio_name);
    if (size == 0) {
        return 0;
    }
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(dst, radio_name, n);
    dst[n] = 0;
    return n;
}

int btc_radio_set_visibility(bool disc, bool conn) {
    discoverable = disc;
    connectable = conn;
    if (active) {
        gap_connectable_control(conn);
        gap_discoverable_control(disc);
    }
    return BTC_OK;
}

void btc_radio_visibility(bool *disc, bool *conn) {
    *disc = discoverable;
    *conn = connectable;
}

int btc_radio_set_pin(const char *pin) {
    size_t n = strlen(pin);
    if (n < 1 || n > 16) {
        return BTC_ERR_ARG;
    }
    memcpy(radio_pin, pin, n + 1);
    return BTC_OK;
}

size_t btc_radio_pin(char *dst, size_t size) {
    size_t n = strlen(radio_pin);
    if (size == 0) {
        return 0;
    }
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(dst, radio_pin, n);
    dst[n] = 0;
    return n;
}

int btc_radio_mac(btc_addr_t mac) {
    if (!active) {
        return BTC_ERR_STATE;
    }
    gap_local_bd_addr(mac);
    return BTC_OK;
}

int btc_radio_paired(btc_addr_t *addrs, int max) {
    if (!active) {
        store_load();
    }
    int n = 0;
    for (int i = 0; i < MAX_KEYS; i++) {
        if (store.keys[i].type) {
            if (n < max) {
                memcpy(addrs[n], store.keys[i].addr, 6);
            }
            n++;
        }
    }
    return n;
}

int btc_radio_unpair(const uint8_t *addr) {
    if (!active) {
        store_load();
    }
    if (addr == NULL) {
        for (int i = 0; i < MAX_KEYS; i++) {
            if (store.keys[i].type) {
                if (active) {
                    gap_drop_link_key_for_bd_addr(store.keys[i].addr);
                }
                memset(&store.keys[i], 0, sizeof(store.keys[i]));
            }
        }
        store.has_last_peer = 0;
    } else {
        int i = key_find(addr);
        if (i < 0) {
            return BTC_OK;  // not paired: nothing to do, as on the ESP32
        }
        if (active) {
            gap_drop_link_key_for_bd_addr(store.keys[i].addr);
        }
        memset(&store.keys[i], 0, sizeof(store.keys[i]));
    }
    store_save();
    radio_changed |= BTC_CHANGED_PAIRED;
    return BTC_OK;
}

uint32_t btc_radio_changes(void) {
    uint32_t c = radio_changed;
    radio_changed = 0;
    return c;
}

int btc_radio_inquiry(uint32_t duration_ms) {
    sync_host();
    if (!active) {
        return BTC_ERR_STATE;
    }
    if (duration_ms == 0) {
        if (scanning) {
            gap_inquiry_stop();
        }
        scanning = false;
        if (inquiring) {
            inquiring = false;
            radio_changed |= BTC_CHANGED_FOUND;
        }
        return BTC_OK;
    }
    if (inquiring) {
        return BTC_ERR_STATE;
    }
    uint32_t units = (duration_ms + 1279) / 1280;
    if (units < 1) {
        units = 1;
    }
    if (units > 48) {
        units = 48;
    }
    found_count = 0;
    if (gap_inquiry_start((uint8_t)units) != ERROR_CODE_SUCCESS) {
        return BTC_ERR_FAIL;
    }
    inquiring = scanning = true;
    naming = -1;
    return BTC_OK;
}

bool btc_radio_inquiring(void) {
    return inquiring;
}

int btc_radio_found(btc_found_t *out, int max) {
    int n = found_count < max ? found_count : max;
    if (n > 0) {
        memcpy(out, found, n * sizeof(*out));
    }
    return found_count;
}
