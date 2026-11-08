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
} r = {
    .discoverable = true,
    .connectable = true,
    .pin = "0000",
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
        case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
            if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
                btc_sink_on_rssi_delta(param->read_rssi_delta.bda, param->read_rssi_delta.rssi_delta);
            }
            break;
        default:
            break;
    }
}

// ---- on and off ----

int btc_radio_start(void) {
    if (r.on) {
        return BTC_OK;
    }
    esp_err_t err = ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        // Classic only: give the BLE half of the controller's memory back.
        // Harmless if it has gone already.
        esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
    }
    if (err == ESP_OK && esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
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
        return btc_err(err);
    }
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
    return BTC_OK;
}

void btc_radio_stop(void) {
    if (!r.on) {
        return;
    }
    btc_hf_stop();
    btc_sink_stop();
    btc_source_stop();
    btc_spp_stop();
    btc_hid_stop();
    btc_inquiry_stop();
    r.on = false;
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
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
