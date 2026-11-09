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
    if (profiles & BTC_PROFILE_PUSH) {
        // Object Transfer: a phone's share sheet lists only devices with it.
        service |= 0x100000;
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

static btc_eir_t eir_set;
static void apply_eir(void);

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
    if (eir_set.uuids) {
        apply_eir();
    }
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
// Pairing: the IO capability, and the step waiting on the program.
static int radio_io = BTC_IO_NONE;
static btc_passkey_t radio_passkey;

static void passkey_step(const bd_addr_t addr, int action, uint32_t passkey) {
    memcpy(radio_passkey.addr, addr, 6);
    radio_passkey.action = action;
    radio_passkey.passkey = passkey;
    radio_changed |= BTC_CHANGED_PASSKEY;
}

static void apply_io(void) {
    // BTstack's SSP_IO_CAPABILITY_* are HCI's values, as BTC_IO_* are.
    gap_ssp_set_io_capability(radio_io);
    // "Just works" and a display-only board answer by themselves.
    gap_ssp_set_auto_accept(radio_io == BTC_IO_NONE || radio_io == BTC_IO_DISPLAY_ONLY);
    // With a way to check, ask for it: services then need MITM protection.
    gap_set_security_level(radio_io == BTC_IO_NONE ? LEVEL_2 : LEVEL_3);
}
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

// ---- link quality ----

static uint32_t page_timeout_ms = 15360;    // BTstack's own, 0x6000 slots
static uint8_t channels[BTC_CHANNEL_MAP_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
static bool channels_pending;               // to send when HCI is free
static uint8_t eir_data[240];               // BTstack reads it from here

static hci_con_handle_t rssi_handle = HCI_CON_HANDLE_INVALID;
static int rssi_state = BTC_ERR_STATE;      // 0 waiting, 1 answered, BTC_ERR_*
static int rssi_delta;

// Set AFH Host Channel Classification, which BTstack has no command for.
static void send_channels(void) {
    if (channels_pending && hci_can_send_command_packet_now()) {
        channels_pending = false;
        uint8_t cmd[3 + BTC_CHANNEL_MAP_LEN] = {0x3f, 0x0c, BTC_CHANNEL_MAP_LEN};
        memcpy(cmd + 3, channels, BTC_CHANNEL_MAP_LEN);
        hci_send_cmd_packet(cmd, sizeof(cmd));
    }
}

static int eir_field(int at, uint8_t type, const uint8_t *data, int len) {
    if (at + 2 + len > (int)sizeof(eir_data)) {
        return at;
    }
    eir_data[at] = (uint8_t)(len + 1);
    eir_data[at + 1] = type;
    memcpy(eir_data + at + 2, data, len);
    return at + 2 + len;
}

// The inquiry response: the name, then what config() added. BTstack sends
// the name alone while none is set.
static void apply_eir(void) {
    if (!active) {
        return;
    }
    if (!eir_set.uuids && !eir_set.manufacturer_len && !eir_set.url_len) {
        gap_set_extended_inquiry_response(NULL);
        return;
    }
    memset(eir_data, 0, sizeof(eir_data));
    int at = 0;
    if (eir_set.uuids) {
        uint16_t uuids[8];
        int n = 0;
        if (profiles & BTC_PROFILE_SINK) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_AUDIO_SINK;
        }
        if (profiles & BTC_PROFILE_SOURCE) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_AUDIO_SOURCE;
        }
        if (profiles & (BTC_PROFILE_SINK | BTC_PROFILE_SOURCE)) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_AV_REMOTE_CONTROL;
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_AV_REMOTE_CONTROL_TARGET;
        }
        if (profiles & BTC_PROFILE_HF) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_HANDSFREE;
        }
        if (profiles & BTC_PROFILE_SERIAL) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_SERIAL_PORT;
        }
        if (profiles & BTC_PROFILE_HID_DEVICE) {
            uuids[n++] = BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE;
        }
        uint8_t le[16];
        for (int i = 0; i < n; i++) {
            little_endian_store_16(le, 2 * i, uuids[i]);
        }
        at = eir_field(at, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS, le, 2 * n);
    }
    if (eir_set.manufacturer_len) {
        at = eir_field(at, BLUETOOTH_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA, eir_set.manufacturer, eir_set.manufacturer_len);
    }
    if (eir_set.url_len) {
        at = eir_field(at, BLUETOOTH_DATA_TYPE_URI, eir_set.url, eir_set.url_len);
    }
    // The name last, shortened to what room is left.
    int room = (int)sizeof(eir_data) - at - 2;
    int n = (int)strlen(radio_name);
    if (room > 0) {
        eir_field(at, n > room ? BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME : BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME,
            (const uint8_t *)radio_name, n > room ? room : n);
    }
    gap_set_extended_inquiry_response(eir_data);
}

static void hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    send_inquiry_mode();
    send_channels();
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
        case HCI_EVENT_LINK_KEY_NOTIFICATION: {
            // BTstack keeps a new key only when the other side asked for
            // bonding. A device the user paired the board with is paired
            // whatever it asked, so keep the key either way, or paired()
            // and the next connection would not know it.
            hci_event_link_key_request_get_bd_addr(packet, addr);
            link_key_type_t type = (link_key_type_t)packet[24];
            if (type != CHANGED_COMBINATION_KEY && !btstack_is_null(&packet[8], 16)) {
                int i = key_find(addr);
                if (i < 0 || memcmp(store.keys[i].key, &packet[8], 16) != 0) {
                    db_put_link_key(addr, &packet[8], type);
                }
            }
            break;
        }
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            uint32_t n = hci_event_user_confirmation_request_get_numeric_value(packet);
            hci_event_user_confirmation_request_get_bd_addr(packet, addr);
            if (radio_io == BTC_IO_DISPLAY_YESNO) {
                passkey_step(addr, BTC_PASSKEY_COMPARE, n);
            } else if (radio_io == BTC_IO_DISPLAY_ONLY) {
                passkey_step(addr, BTC_PASSKEY_DISPLAY, n);
            } else if (radio_io == BTC_IO_KEYBOARD_ONLY) {
                gap_ssp_confirmation_response(addr);
            }
            btc_log("ssp confirm %06u", (unsigned)n);
            break;
        }
        case HCI_EVENT_USER_PASSKEY_REQUEST:
            hci_event_user_passkey_request_get_bd_addr(packet, addr);
            passkey_step(addr, BTC_PASSKEY_INPUT, 0);
            break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            hci_event_user_passkey_notification_get_bd_addr(packet, addr);
            passkey_step(addr, BTC_PASSKEY_DISPLAY, hci_event_user_passkey_notification_get_numeric_value(packet));
            break;
        case GAP_EVENT_PAIRING_COMPLETE:
            radio_passkey.action = 0;
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
        case GAP_EVENT_RSSI_MEASUREMENT:
            if (rssi_state == 0 && gap_event_rssi_measurement_get_con_handle(packet) == rssi_handle) {
                rssi_delta = (int8_t)gap_event_rssi_measurement_get_rssi(packet);
                rssi_state = 1;
            }
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
        btc_l2cap_radio_lost();
        btc_hf_radio_lost();
        btc_sink_radio_lost();
        btc_source_radio_lost();
        btc_hid_radio_lost();
        btc_pb_radio_lost();
        btc_lib_radio_lost();
        btc_tether_radio_lost();
        btc_opp_radio_lost();
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
    apply_io();
    gap_set_bondable_mode(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH | LM_LINK_POLICY_ENABLE_SNIFF_MODE);
    gap_set_allow_role_switch(true);

    sdp_init();

    active = true;
    gap_set_page_timeout((uint16_t)((page_timeout_ms * 8 + 4) / 5));
    if (memcmp(channels, (const uint8_t[]){0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}, BTC_CHANNEL_MAP_LEN) != 0) {
        channels_pending = true;
        send_channels();
    }
    apply_eir();
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
    btc_l2cap_radio_stopping();
    btc_hf_radio_stopping();
    btc_sink_radio_stopping();
    btc_source_radio_stopping();
    btc_hid_radio_stopping();
    btc_pb_radio_stopping();
    btc_lib_radio_stopping();
    btc_tether_radio_stopping();
    btc_opp_radio_stopping();
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
        apply_eir();
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

int btc_radio_set_io(int io) {
    if (io < BTC_IO_DISPLAY_ONLY || io > BTC_IO_NONE) {
        return BTC_ERR_ARG;
    }
    radio_io = io;
    if (btc_radio_active()) {
        apply_io();
    }
    return BTC_OK;
}

int btc_radio_io(void) {
    return radio_io;
}

void btc_radio_passkey(btc_passkey_t *out) {
    *out = radio_passkey;
}

int btc_radio_passkey_reply(bool accept, uint32_t passkey) {
    btc_passkey_t p = radio_passkey;
    if (p.action != BTC_PASSKEY_INPUT && p.action != BTC_PASSKEY_COMPARE) {
        return BTC_ERR_STATE;
    }
    radio_passkey.action = 0;
    bd_addr_t a;
    memcpy(a, p.addr, 6);
    uint8_t s;
    if (p.action == BTC_PASSKEY_INPUT) {
        s = accept ? gap_ssp_passkey_response(a, passkey) : gap_ssp_passkey_negative(a);
    } else {
        s = accept ? gap_ssp_confirmation_response(a) : gap_ssp_confirmation_negative(a);
    }
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
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

// ---- services ----

// One SDP query for every record that runs over L2CAP (that is, every
// service), asking for each one's ServiceClassIDList. Its first UUID is
// the record's most specific class.
static int sdp_state = BTC_ERR_STATE;   // 0 searching, 1 ended, BTC_ERR_*
static int sdp_count;
static btc_uuid_t sdp_found[BTC_SERVICES_MAX];
static uint8_t sdp_attr[64];            // the attribute value being received
static bd_addr_t sdp_addr;

static void sdp_take_class(void) {
    if (sdp_count >= BTC_SERVICES_MAX || de_get_element_type(sdp_attr) != DE_DES) {
        return;
    }
    des_iterator_t it;
    for (des_iterator_init(&it, sdp_attr); des_iterator_has_more(&it); des_iterator_next(&it)) {
        if (des_iterator_get_type(&it) != DE_UUID) {
            continue;
        }
        const uint8_t *e = des_iterator_get_element(&it);
        btc_uuid_t *o = &sdp_found[sdp_count];
        switch (de_get_size_type(e)) {
            case DE_SIZE_16:
                o->len = 2;
                break;
            case DE_SIZE_32:
                o->len = 4;
                break;
            case DE_SIZE_128:
                o->len = 16;
                break;
            default:
                return;
        }
        memcpy(o->uuid, e + 1, o->len);
        // The same class from two records is listed once.
        for (int i = 0; i < sdp_count; i++) {
            if (sdp_found[i].len == o->len && memcmp(sdp_found[i].uuid, o->uuid, o->len) == 0) {
                return;
            }
        }
        sdp_count++;
        return;
    }
}

static void on_sdp_services(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET || sdp_state != 0) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            uint16_t len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            uint16_t at = sdp_event_query_attribute_byte_get_data_offset(packet);
            if (len > sizeof(sdp_attr)) {
                break;
            }
            sdp_attr[at] = sdp_event_query_attribute_byte_get_data(packet);
            if (at + 1 == len) {
                sdp_take_class();
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE:
            sdp_state = sdp_event_query_complete_get_status(packet) == ERROR_CODE_SUCCESS ? 1 : BTC_ERR_FAIL;
            break;
        default:
            break;
    }
}

int btc_radio_query_services(const uint8_t *addr) {
    static const uint8_t pattern[] = { 0x35, 0x03, 0x19, 0x01, 0x00 };     // L2CAP
    static const uint8_t attributes[] = { 0x35, 0x03, 0x09, 0x00, 0x01 };  // ServiceClassIDList
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    // The SDP client takes one query at a time, and says so.
    memcpy(sdp_addr, addr, 6);
    sdp_count = 0;
    sdp_state = 0;
    if (sdp_client_query(on_sdp_services, sdp_addr, pattern, attributes) != ERROR_CODE_SUCCESS) {
        // A search still running, or Serial's own lookup, holds the client.
        sdp_state = BTC_ERR_STATE;
        return BTC_ERR_STATE;
    }
    return BTC_OK;
}

int btc_radio_services(btc_uuid_t *out, int max, int *count) {
    int n = sdp_count < max ? sdp_count : max;
    *count = 0;
    if (sdp_state == 1) {
        memcpy(out, sdp_found, n * sizeof(btc_uuid_t));
        *count = n;
    }
    return sdp_state;
}

int btc_radio_read_rssi(const uint8_t *addr) {
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    bd_addr_t a;
    memcpy(a, addr, 6);
    hci_connection_t *c = hci_connection_for_bd_addr_and_type(a, BD_ADDR_TYPE_ACL);
    if (c == NULL) {
        rssi_state = BTC_ERR_STATE;
        return BTC_ERR_STATE;
    }
    rssi_handle = c->con_handle;
    rssi_state = 0;
    // gap_read_rssi() answers 1 when it queued the command and 0 when it
    // knows no such link, not an ERROR_CODE_ status.
    if (!gap_read_rssi(rssi_handle)) {
        rssi_state = BTC_ERR_FAIL;
        return BTC_ERR_FAIL;
    }
    return BTC_OK;
}

int btc_radio_rssi(int *delta) {
    *delta = rssi_delta;
    return rssi_state;
}

// SDP records of the program's own are Bluedroid's for now too.
int btc_service_add(const btc_uuid_t *uuid, const char *name, int channel, int psm) {
    (void)uuid;
    (void)name;
    (void)channel;
    (void)psm;
    return BTC_ERR_UNSUPPORTED;
}

bool btc_service_active(int slot) {
    (void)slot;
    return false;
}

void btc_service_remove(int slot) {
    (void)slot;
}

// Link tuning is Bluedroid's for now: BTstack has the HCI commands but
// nothing here has run them on a CYW43439.
int btc_radio_set_min_key_size(int bytes) {
    (void)bytes;
    return BTC_ERR_UNSUPPORTED;
}

int btc_radio_min_key_result(void) {
    return BTC_ERR_UNSUPPORTED;
}

int btc_radio_min_key_size(void) {
    return BTC_KEY_SIZE_MIN;
}

int btc_radio_set_poll(const uint8_t *addr, uint32_t ms) {
    (void)addr;
    (void)ms;
    return BTC_ERR_UNSUPPORTED;
}

int btc_radio_set_packet_types(const uint8_t *addr, uint32_t types) {
    (void)addr;
    (void)types;
    return BTC_ERR_UNSUPPORTED;
}

int btc_radio_tune_result(uint32_t *value) {
    *value = 0;
    return BTC_ERR_UNSUPPORTED;
}

int btc_radio_set_page_timeout(uint32_t ms) {
    if (ms < 1 || ms > 40959) {
        return BTC_ERR_ARG;
    }
    page_timeout_ms = ms;
    if (btc_radio_active()) {
        gap_set_page_timeout((uint16_t)((ms * 8 + 4) / 5));
    }
    return BTC_OK;
}

uint32_t btc_radio_page_timeout(void) {
    return page_timeout_ms;
}

int btc_radio_set_channels(const uint8_t *map) {
    int count = 0;
    for (int ch = 0; ch < 79; ch++) {
        count += (map[ch / 8] >> (ch % 8)) & 1;
    }
    if (count < 20 || (map[BTC_CHANNEL_MAP_LEN - 1] & 0x80)) {
        return BTC_ERR_ARG;
    }
    memcpy(channels, map, BTC_CHANNEL_MAP_LEN);
    if (btc_radio_active()) {
        channels_pending = true;
        send_channels();
    }
    return BTC_OK;
}

void btc_radio_channels(uint8_t *map) {
    memcpy(map, channels, BTC_CHANNEL_MAP_LEN);
}

int btc_radio_set_eir(const btc_eir_t *eir) {
    if (eir->tx_power) {
        return BTC_ERR_UNSUPPORTED;
    }
    if (eir->manufacturer_len > BTC_EIR_DATA_MAX || eir->url_len > BTC_EIR_DATA_MAX
        || (eir->manufacturer_len && eir->manufacturer_len < 2)) {
        return BTC_ERR_ARG;
    }
    eir_set = *eir;
    if (btc_radio_active()) {
        apply_eir();
    }
    return BTC_OK;
}

void btc_radio_eir(btc_eir_t *eir) {
    *eir = eir_set;
}

int btc_radio_found_eir(const uint8_t *addr, uint8_t *dst, size_t size) {
    (void)addr;
    (void)dst;
    (void)size;
    return BTC_ERR_UNSUPPORTED;
}
