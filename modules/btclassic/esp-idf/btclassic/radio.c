// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The radio on Bluedroid: the controller and the host, the board's name and
// visibility, and pairing with nothing to type. See include/btclassic.h.

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_bt.h"
#include "esp_timer.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "btc_bluedroid.h"

static const char *TAG = "btclassic";

portMUX_TYPE btc_lock = portMUX_INITIALIZER_UNLOCKED;

#define BT_NAME_MAX 248    // ESP_BT_GAP_MAX_BDNAME_LEN
#define BT_PIN_MAX 16

static struct {
    bool on;
    bool discoverable;
    bool connectable;
    bool hidden;        // a profile holds its one device
    char name[BT_NAME_MAX + 1];
    char pin[BT_PIN_MAX + 1];
    uint32_t changes;
    uint32_t profiles;  // BTC_PROFILE_* bits of the profiles running
    uint8_t hid_subclass;
    uint32_t class_set; // the class asked for in the settings, or 0
    uint32_t page_timeout_ms;
    uint8_t channels[BTC_CHANNEL_MAP_LEN];
    btc_eir_t eir;
    // A signal reading asked for by address: where it stands, and the answer.
    btc_addr_t rssi_addr;
    int rssi_state;     // 0 waiting, 1 answered, BTC_ERR_* refused
    int rssi_delta;
} r = {
    .discoverable = true,
    .connectable = true,
    .pin = "0000",
    .page_timeout_ms = 5120,    // the controller's own, 0x2000 slots
    .channels = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    .rssi_state = BTC_ERR_STATE,
};

int btc_err(esp_err_t err) {
    switch (err) {
        case ESP_OK:
            return BTC_OK;
        case ESP_ERR_NO_MEM:
            return BTC_ERR_NOMEM;
        case ESP_ERR_INVALID_STATE:
            return BTC_ERR_STATE;
        case ESP_ERR_NOT_SUPPORTED:
            return BTC_ERR_UNSUPPORTED;
        case ESP_ERR_INVALID_ARG:
            return BTC_ERR_ARG;
        default:
            return BTC_ERR_FAIL;
    }
}

void btc_set_text(char *field, const uint8_t *src, size_t len) {
    if (len >= BTC_TEXT_MAX) {
        len = BTC_TEXT_MAX - 1;
    }
    BTC_LOCK();
    memcpy(field, src, len);
    field[len] = 0;
    BTC_UNLOCK();
}

size_t btc_get_text(const char *field, char *dst, size_t size) {
    if (size == 0) {
        return 0;
    }
    BTC_LOCK();
    strncpy(dst, field, size - 1);
    BTC_UNLOCK();
    dst[size - 1] = 0;
    return strlen(dst);
}

static void mark(uint32_t bits) {
    BTC_LOCK();
    r.changes |= bits;
    BTC_UNLOCK();
}

void btc_radio_mark(uint32_t bits) {
    mark(bits);
}

static void apply_scan_mode(void) {
    if (!r.on) {
        return;
    }
    bool connectable = r.connectable && !r.hidden;
    bool discoverable = connectable && r.discoverable;
    esp_bt_gap_set_scan_mode(connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

void btc_radio_hold_hidden(bool hidden) {
    r.hidden = hidden;
    apply_scan_mode();
}

static void apply_pin(void) {
    esp_bt_pin_code_t pin = {0};
    size_t len = strlen(r.pin);
    memcpy(pin, r.pin, len);
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, len, pin);
}

// ---- GAP: pairing with no PIN to type, a peer's name and signal ----

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    // Inquiry results, and remote names for the source.
    btc_inquiry_on_gap(event, param);
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            btc_sink_on_auth(param->auth_cmpl.bda);
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
                mark(BTC_CHANGED_PAIRED);
            } else {
                ESP_LOGW(TAG, "pairing failed: %d", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            // A board has no screen to compare numbers on: accept.
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;
        case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
            mark(BTC_CHANGED_PAIRED);
            break;
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                btc_sink_on_remote_name(param->read_rmt_name.bda, (const char *)param->read_rmt_name.rmt_name);
            }
            break;
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT: {
            bool ok = param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS;
            if (ok) {
                btc_sink_on_rssi_delta(param->read_rssi_delta.bda, param->read_rssi_delta.rssi_delta);
            }
            BTC_LOCK();
            if (r.rssi_state == 0 && memcmp(r.rssi_addr, param->read_rssi_delta.bda, sizeof(btc_addr_t)) == 0) {
                r.rssi_delta = param->read_rssi_delta.rssi_delta;
                r.rssi_state = ok ? 1 : BTC_ERR_STATE;
            }
            BTC_UNLOCK();
            break;
        }
        case ESP_BT_GAP_SET_AFH_CHANNELS_EVT:
            if (param->set_afh_channels.stat != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "channel map refused: %d", param->set_afh_channels.stat);
            }
            break;
        case ESP_BT_GAP_SET_PAGE_TO_EVT:
            if (param->set_page_timeout.stat != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "page timeout refused: %d", param->set_page_timeout.stat);
            }
            break;
        case ESP_BT_GAP_CONFIG_EIR_DATA_EVT:
            if (param->config_eir_data.stat != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "inquiry response refused: %d", param->config_eir_data.stat);
            }
            break;
        default:
            break;
    }
}

// ---- Class of Device ----

// Each Bluedroid profile writes the Class of Device as it starts and stops,
// and the last one wins: a speaker that also runs a keyboard could show as
// a keyboard, or lose its Audio bit when Serial starts. So the class is
// worked out here from every profile running, and written a little after
// the profiles' own writes have landed.

#define CLASS_DELAY_US 300000

static esp_timer_handle_t class_timer;

static uint32_t auto_class(void) {
    uint32_t p = r.profiles;
    uint32_t service = 0, major, minor;
    if (p & BTC_PROFILE_SINK) {
        service |= ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING;
    }
    if (p & BTC_PROFILE_SOURCE) {
        service |= ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_CAPTURING;
    }
    if (p & BTC_PROFILE_HF) {
        service |= ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
    }
    if (p & BTC_PROFILE_AG) {
        service |= ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
    }
    if (p & BTC_PROFILE_AG) {
        // The board is the phone: headsets and car kits look for one.
        major = ESP_BT_COD_MAJOR_DEV_PHONE;
        minor = 0x03;                            // smartphone
    } else if (p & (BTC_PROFILE_SINK | BTC_PROFILE_SOURCE | BTC_PROFILE_HF)) {
        // Audio/Video, even with a keyboard: a computer offers audio by it.
        major = ESP_BT_COD_MAJOR_DEV_AV;
        minor = (p & BTC_PROFILE_HF) ? 0x02      // hands-free
            : (p & BTC_PROFILE_SINK) ? 0x05      // loudspeaker
            : 0x00;                              // unclassified
    } else if (p & BTC_PROFILE_HID_DEVICE) {
        major = ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
        minor = r.hid_subclass >> 2;
    } else {
        major = ESP_BT_COD_MAJOR_DEV_UNCATEGORIZED;
        minor = 0;
    }
    return service << ESP_BT_COD_SRVC_BIT_OFFSET
           | major << ESP_BT_COD_MAJOR_DEV_BIT_OFFSET
           | minor << ESP_BT_COD_MINOR_DEV_BIT_OFFSET;
}

uint32_t btc_radio_class(void) {
    return r.class_set ? r.class_set : auto_class();
}

static void apply_class(void *arg) {
    (void)arg;
    if (!r.on) {
        return;
    }
    uint32_t c = btc_radio_class();
    esp_bt_cod_t cod = {0};
    cod.minor = (c & ESP_BT_COD_MINOR_DEV_BIT_MASK) >> ESP_BT_COD_MINOR_DEV_BIT_OFFSET;
    cod.major = (c & ESP_BT_COD_MAJOR_DEV_BIT_MASK) >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET;
    cod.service = (c & ESP_BT_COD_SRVC_BIT_MASK) >> ESP_BT_COD_SRVC_BIT_OFFSET;
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
}

static void schedule_class(void) {
    if (!r.on) {
        return;
    }
    if (class_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = apply_class, .name = "btclassic_cod"};
        if (esp_timer_create(&args, &class_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(class_timer);
    esp_timer_start_once(class_timer, CLASS_DELAY_US);
}

void btc_radio_profile(uint32_t profile, bool running, uint8_t hid_subclass) {
    if (running) {
        r.profiles |= profile;
        if (profile & BTC_PROFILE_HID_DEVICE) {
            r.hid_subclass = hid_subclass;
        }
    } else {
        r.profiles &= ~profile;
    }
    schedule_class();
}

int btc_radio_set_class(uint32_t cod) {
    if (cod > 0xffffff || (cod & 3)) {
        return BTC_ERR_ARG;
    }
    r.class_set = cod;
    schedule_class();
    return BTC_OK;
}

// ---- link settings ----

static void apply_page_timeout(void) {
    // In slots of 0.625 ms.
    esp_bt_gap_set_page_timeout((uint16_t)((r.page_timeout_ms * 8 + 4) / 5));
}

static bool channels_default(void) {
    for (int i = 0; i < BTC_CHANNEL_MAP_LEN; i++) {
        if (r.channels[i] != (i == BTC_CHANNEL_MAP_LEN - 1 ? 0x7f : 0xff)) {
            return false;
        }
    }
    return true;
}

static void apply_channels(void) {
    esp_bt_gap_afh_channels map;
    memcpy(map, r.channels, sizeof(map));
    esp_bt_gap_set_afh_channels(map);
}

static void apply_eir(void) {
    esp_bt_eir_data_t eir = {
        .fec_required = true,
        .include_name = true,
        .include_uuid = r.eir.uuids,
        .include_txpower = r.eir.tx_power,
        .manufacturer_len = r.eir.manufacturer_len,
        .p_manufacturer_data = r.eir.manufacturer_len ? r.eir.manufacturer : NULL,
        .url_len = r.eir.url_len,
        .p_url = r.eir.url_len ? r.eir.url : NULL,
    };
    esp_bt_gap_config_eir_data(&eir);
}

static bool eir_default(void) {
    return !r.eir.uuids && !r.eir.tx_power && !r.eir.manufacturer_len && !r.eir.url_len;
}

// ---- on and off ----

// The controller and Bluedroid serve two users: Classic (this file's
// radio) and BLE (MicroPython's `bluetooth` module). They come up with the
// first and go down with the last.
#define USER_CLASSIC 1
#define USER_BLE 2
static uint8_t users;

#if CONFIG_BTDM_CTRL_MODE_BTDM
#define CONTROLLER_MODE ESP_BT_MODE_BTDM
#else
#define CONTROLLER_MODE ESP_BT_MODE_CLASSIC_BT
#endif

static int stack_up(void) {
    esp_err_t err = ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        #if !CONFIG_BTDM_CTRL_MODE_BTDM
        // Classic only: give the BLE half of the controller's memory back.
        // Harmless if it has gone already.
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        #endif
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
    }
    if (err == ESP_OK && esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(CONTROLLER_MODE);
    }
    if (err == ESP_OK && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        err = esp_bluedroid_init_with_cfg(&bcfg);
    }
    if (err == ESP_OK && esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        err = esp_bluedroid_enable();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth didn't start: %s", esp_err_to_name(err));
    }
    return btc_err(err);
}

static void stack_down(void) {
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
}

int btc_stack_ble_start(void) {
    if (users & USER_BLE) {
        return BTC_OK;
    }
    if (!users) {
        int err = stack_up();
        if (err != BTC_OK) {
            return err;
        }
    }
    users |= USER_BLE;
    return BTC_OK;
}

void btc_stack_ble_stop(void) {
    if (!(users & USER_BLE)) {
        return;
    }
    users &= ~USER_BLE;
    if (!users) {
        stack_down();
    }
}

int btc_radio_start(void) {
    if (r.on) {
        return BTC_OK;
    }
    if (!users) {
        int err = stack_up();
        if (err != BTC_OK) {
            return err;
        }
    }
    users |= USER_CLASSIC;
    esp_bt_gap_set_device_name(r.name[0] ? r.name : "ESP32");
    esp_bt_gap_register_callback(gap_cb);
    // Secure Simple Pairing with no display or keyboard: "just works".
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    // Legacy pairing, for devices without SSP.
    apply_pin();
    r.on = true;
    r.hidden = false;
    apply_scan_mode();
    schedule_class();
    // Settings made before start, where they differ from the stack's own.
    if (r.page_timeout_ms != 5120) {
        apply_page_timeout();
    }
    if (!channels_default()) {
        apply_channels();
    }
    if (!eir_default()) {
        apply_eir();
    }
    return BTC_OK;
}

void btc_radio_stop(void) {
    if (!r.on) {
        return;
    }
    btc_msg_stop();
    btc_pb_stop();
    btc_ag_stop();
    btc_hf_stop();
    btc_sink_stop();
    btc_source_stop();
    btc_spp_stop();
    btc_hid_stop();
    btc_inquiry_stop();
    if (class_timer) {
        esp_timer_stop(class_timer);
        esp_timer_delete(class_timer);
        class_timer = NULL;
    }
    r.on = false;
    users &= ~USER_CLASSIC;
    if (!users) {
        stack_down();
    } else {
        // BLE keeps the stack: just stop being found or connected.
        esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
}

bool btc_radio_active(void) {
    return r.on;
}

// ---- settings ----

int btc_radio_set_name(const char *name) {
    size_t len = strlen(name);
    if (len > BT_NAME_MAX) {
        return BTC_ERR_ARG;
    }
    memcpy(r.name, name, len + 1);
    if (r.on) {
        return btc_err(esp_bt_gap_set_device_name(r.name));
    }
    return BTC_OK;
}

size_t btc_radio_name(char *dst, size_t size) {
    if (size == 0) {
        return 0;
    }
    strncpy(dst, r.name, size - 1);
    dst[size - 1] = 0;
    return strlen(dst);
}

int btc_radio_set_visibility(bool discoverable, bool connectable) {
    r.discoverable = discoverable;
    r.connectable = connectable;
    apply_scan_mode();
    return BTC_OK;
}

void btc_radio_visibility(bool *discoverable, bool *connectable) {
    *discoverable = r.discoverable;
    *connectable = r.connectable;
}

int btc_radio_set_pin(const char *pin) {
    size_t len = strlen(pin);
    if (len < 1 || len > BT_PIN_MAX) {
        return BTC_ERR_ARG;
    }
    memcpy(r.pin, pin, len + 1);
    if (r.on) {
        apply_pin();
    }
    return BTC_OK;
}

size_t btc_radio_pin(char *dst, size_t size) {
    if (size == 0) {
        return 0;
    }
    strncpy(dst, r.pin, size - 1);
    dst[size - 1] = 0;
    return strlen(dst);
}

int btc_radio_mac(btc_addr_t mac) {
    return btc_err(esp_read_mac(mac, ESP_MAC_BT));
}

// ---- pairings ----

int btc_radio_paired(btc_addr_t *addrs, int max) {
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0 || max <= 0) {
        return n < 0 ? 0 : n;
    }
    esp_bd_addr_t *all = malloc(n * sizeof(esp_bd_addr_t));
    if (all == NULL) {
        return BTC_ERR_NOMEM;
    }
    int got = n;
    if (esp_bt_gap_get_bond_device_list(&got, all) != ESP_OK) {
        got = 0;
    }
    for (int i = 0; i < got && i < max; i++) {
        memcpy(addrs[i], all[i], sizeof(btc_addr_t));
    }
    free(all);
    return got;
}

int btc_radio_unpair(const uint8_t *addr) {
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    if (addr) {
        esp_bd_addr_t a;
        memcpy(a, addr, sizeof(a));
        return btc_err(esp_bt_gap_remove_bond_device(a));
    }
    int n = btc_radio_paired(NULL, 0);
    if (n <= 0) {
        return n;
    }
    btc_addr_t *all = malloc(n * sizeof(btc_addr_t));
    if (all == NULL) {
        return BTC_ERR_NOMEM;
    }
    n = btc_radio_paired(all, n);
    for (int i = 0; i < n; i++) {
        esp_bt_gap_remove_bond_device(all[i]);
    }
    free(all);
    return BTC_OK;
}

uint32_t btc_radio_changes(void) {
    BTC_LOCK();
    uint32_t c = r.changes;
    r.changes = 0;
    BTC_UNLOCK();
    return c;
}

// ---- link quality ----

int btc_radio_read_rssi(const uint8_t *addr) {
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    esp_bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    BTC_LOCK();
    memcpy(r.rssi_addr, addr, sizeof(btc_addr_t));
    r.rssi_state = 0;
    BTC_UNLOCK();
    int err = btc_err(esp_bt_gap_read_rssi_delta(a));
    if (err != BTC_OK) {
        BTC_LOCK();
        r.rssi_state = err;
        BTC_UNLOCK();
    }
    return err;
}

int btc_radio_rssi(int *delta) {
    BTC_LOCK();
    int state = r.rssi_state;
    *delta = r.rssi_delta;
    BTC_UNLOCK();
    return state;
}

int btc_radio_set_page_timeout(uint32_t ms) {
    if (ms < 1 || ms > 40959) {
        return BTC_ERR_ARG;
    }
    r.page_timeout_ms = ms;
    if (r.on) {
        apply_page_timeout();
    }
    return BTC_OK;
}

uint32_t btc_radio_page_timeout(void) {
    return r.page_timeout_ms;
}

int btc_radio_set_channels(const uint8_t *map) {
    int count = 0;
    for (int ch = 0; ch < 79; ch++) {
        count += (map[ch / 8] >> (ch % 8)) & 1;
    }
    if (count < 20 || (map[BTC_CHANNEL_MAP_LEN - 1] & 0x80)) {
        return BTC_ERR_ARG;
    }
    memcpy(r.channels, map, BTC_CHANNEL_MAP_LEN);
    if (r.on) {
        apply_channels();
    }
    return BTC_OK;
}

void btc_radio_channels(uint8_t *map) {
    memcpy(map, r.channels, BTC_CHANNEL_MAP_LEN);
}

int btc_radio_set_eir(const btc_eir_t *eir) {
    if (eir->manufacturer_len > BTC_EIR_DATA_MAX || eir->url_len > BTC_EIR_DATA_MAX
        || (eir->manufacturer_len && eir->manufacturer_len < 2)) {
        return BTC_ERR_ARG;
    }
    r.eir = *eir;
    if (r.on) {
        apply_eir();
    }
    return BTC_OK;
}

void btc_radio_eir(btc_eir_t *eir) {
    *eir = r.eir;
}
