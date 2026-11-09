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
    bool hidden;        // a profile is stopping: no new links at all
    bool unlisted;      // a profile holds its one device: out of searches
    char name[BT_NAME_MAX + 1];
    char pin[BT_PIN_MAX + 1];
    uint32_t changes;
    uint32_t profiles;  // BTC_PROFILE_* bits of the profiles running
    uint8_t hid_subclass;
    uint32_t class_set; // the class asked for in the settings, or 0
    uint32_t page_timeout_ms;
    int min_key_size;   // bytes; 7 is the controller's own
    uint8_t channels[BTC_CHANNEL_MAP_LEN];
    btc_eir_t eir;
    // A signal reading asked for by address: where it stands, and the answer.
    btc_addr_t rssi_addr;
    int rssi_state;     // 0 waiting, 1 answered, BTC_ERR_* refused
    int rssi_delta;
    // A link setting (poll interval or packet types) asked for by address.
    btc_addr_t tune_addr;
    int tune_state;     // 0 waiting, 1 answered, BTC_ERR_*
    uint32_t tune_value;
    // A service search, the same way.
    btc_addr_t sdp_addr;
    int sdp_state;      // 0 searching, 1 ended, BTC_ERR_* failed
    int sdp_count;
    btc_uuid_t sdp[BTC_SERVICES_MAX];
    // Pairing: the IO capability, and the step waiting on the program.
    int io;
    btc_passkey_t passkey;
} r = {
    .discoverable = true,
    .connectable = true,
    .pin = "0000",
    .page_timeout_ms = 5120,    // the controller's own, 0x2000 slots
    .min_key_size = BTC_KEY_SIZE_MIN,
    .channels = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f},
    .rssi_state = BTC_ERR_STATE,
    .tune_state = BTC_ERR_STATE,
    .sdp_state = BTC_ERR_STATE,
    .io = BTC_IO_NONE,
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
    bool discoverable = connectable && r.discoverable && !r.unlisted;
    esp_bt_gap_set_scan_mode(connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
}

void btc_radio_hold_hidden(bool hidden) {
    r.hidden = hidden;
    apply_scan_mode();
}

void btc_radio_hold_unlisted(bool unlisted) {
    r.unlisted = unlisted;
    apply_scan_mode();
}

static void apply_pin(void) {
    esp_bt_pin_code_t pin = {0};
    size_t len = strlen(r.pin);
    memcpy(pin, r.pin, len);
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, len, pin);
}

// ---- GAP: pairing, a peer's name and signal ----

static void passkey_step(const uint8_t *bda, int action, uint32_t passkey) {
    BTC_LOCK();
    memcpy(r.passkey.addr, bda, sizeof(btc_addr_t));
    r.passkey.action = action;
    r.passkey.passkey = passkey;
    BTC_UNLOCK();
    mark(BTC_CHANGED_PASSKEY);
}

static void apply_io(void) {
    esp_bt_io_cap_t iocap = (esp_bt_io_cap_t)r.io;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
}

// ---- per-link settings ----

// HCI's ACL packet type field: a set bit allows DM/DH types and forbids EDR
// ones. The API turns that around so every BTC_PKT_* bit means "allowed".
static const struct {
    uint16_t hci;
    uint32_t bit;
    bool forbids;
} pkt_map[] = {
    {0x0008, BTC_PKT_DM1, false},  {0x0010, BTC_PKT_DH1, false},
    {0x0400, BTC_PKT_DM3, false},  {0x0800, BTC_PKT_DH3, false},
    {0x4000, BTC_PKT_DM5, false},  {0x8000, BTC_PKT_DH5, false},
    {0x0002, BTC_PKT_2DH1, true},  {0x0004, BTC_PKT_3DH1, true},
    {0x0100, BTC_PKT_2DH3, true},  {0x0200, BTC_PKT_3DH3, true},
    {0x1000, BTC_PKT_2DH5, true},  {0x2000, BTC_PKT_3DH5, true},
};

static uint16_t pkt_to_hci(uint32_t types) {
    uint16_t hci = 0;
    for (size_t i = 0; i < sizeof(pkt_map) / sizeof(pkt_map[0]); i++) {
        bool allowed = (types & pkt_map[i].bit) || pkt_map[i].bit == BTC_PKT_DM1;
        if (allowed != pkt_map[i].forbids) {
            hci |= pkt_map[i].hci;
        }
    }
    return hci;
}

static uint32_t pkt_from_hci(uint16_t hci) {
    uint32_t types = 0;
    for (size_t i = 0; i < sizeof(pkt_map) / sizeof(pkt_map[0]); i++) {
        bool set = hci & pkt_map[i].hci;
        if (set != pkt_map[i].forbids) {
            types |= pkt_map[i].bit;
        }
    }
    return types;
}

// The controller's answer to a poll interval or packet type request.
static void tune_done(const uint8_t *bda, bool ok, uint32_t value) {
    BTC_LOCK();
    if (r.tune_state == 0 && memcmp(r.tune_addr, bda, sizeof(btc_addr_t)) == 0) {
        r.tune_value = value;
        r.tune_state = ok ? 1 : BTC_ERR_FAIL;
    }
    BTC_UNLOCK();
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    // Inquiry results, and remote names for the source.
    btc_inquiry_on_gap(event, param);
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            BTC_LOCK();
            r.passkey.action = 0;
            BTC_UNLOCK();
            btc_sink_on_auth(param->auth_cmpl.bda);
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "paired with %s", param->auth_cmpl.device_name);
                mark(BTC_CHANGED_PAIRED);
            } else {
                ESP_LOGW(TAG, "pairing failed: %d", param->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            if (r.io != BTC_IO_DISPLAY_YESNO) {
                // No way to answer yes or no: accept, as "just works"
                // does. A board with a display still shows the number,
                // for the other side to check.
                esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
                if (r.io == BTC_IO_DISPLAY_ONLY) {
                    passkey_step(param->cfm_req.bda, BTC_PASSKEY_DISPLAY, param->cfm_req.num_val);
                }
            } else {
                passkey_step(param->cfm_req.bda, BTC_PASSKEY_COMPARE, param->cfm_req.num_val);
            }
            break;
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            passkey_step(param->key_notif.bda, BTC_PASSKEY_DISPLAY, param->key_notif.passkey);
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            passkey_step(param->key_req.bda, BTC_PASSKEY_INPUT, 0);
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
        case ESP_BT_GAP_RMT_SRVCS_EVT: {
            BTC_LOCK();
            if (r.sdp_state == 0 && memcmp(r.sdp_addr, param->rmt_srvcs.bda, sizeof(btc_addr_t)) == 0) {
                if (param->rmt_srvcs.stat != ESP_BT_STATUS_SUCCESS) {
                    // No answer from the device, or no SDP server.
                    r.sdp_state = BTC_ERR_FAIL;
                } else {
                    int n = 0;
                    for (int i = 0; i < param->rmt_srvcs.num_uuids && n < BTC_SERVICES_MAX; i++) {
                        const esp_bt_uuid_t *u = &param->rmt_srvcs.uuid_list[i];
                        btc_uuid_t *o = &r.sdp[n];
                        if (u->len == ESP_UUID_LEN_16) {
                            o->len = 2;
                            o->uuid[0] = u->uuid.uuid16 >> 8;
                            o->uuid[1] = u->uuid.uuid16;
                        } else if (u->len == ESP_UUID_LEN_32) {
                            o->len = 4;
                            for (int b = 0; b < 4; b++) {
                                o->uuid[b] = u->uuid.uuid32 >> (24 - 8 * b);
                            }
                        } else if (u->len == ESP_UUID_LEN_128) {
                            // Here, unlike BLE's, Bluedroid hands 128-bit
                            // UUIDs over most significant byte first: its
                            // SDP layer and btc each reverse them once.
                            o->len = 16;
                            memcpy(o->uuid, u->uuid.uuid128, 16);
                        } else {
                            continue;
                        }
                        n++;
                    }
                    r.sdp_count = n;
                    r.sdp_state = 1;
                }
            }
            BTC_UNLOCK();
            break;
        }
        case ESP_BT_GAP_SET_AFH_CHANNELS_EVT:
            if (param->set_afh_channels.stat != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "channel map refused: %d", param->set_afh_channels.stat);
            }
            break;
        case ESP_BT_GAP_QOS_CMPL_EVT:
            tune_done(param->qos_cmpl.bda, param->qos_cmpl.stat == ESP_BT_STATUS_SUCCESS,
                (param->qos_cmpl.t_poll * 5 + 4) / 8);
            break;
        case ESP_BT_GAP_ACL_PKT_TYPE_CHANGED_EVT:
            tune_done(param->set_acl_pkt_types.bda, param->set_acl_pkt_types.status == ESP_BT_STATUS_SUCCESS,
                pkt_from_hci(param->set_acl_pkt_types.pkt_types));
            break;
        case ESP_BT_GAP_SET_MIN_ENC_KEY_SIZE_EVT:
            if (param->set_min_enc_key_size.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "minimum key size refused: %d", param->set_min_enc_key_size.status);
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
    if (p & BTC_PROFILE_PUSH) {
        service |= ESP_BT_COD_SRVC_OBJ_TRANSFER;
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

static uint32_t wanted_class(void) {
    return r.class_set ? r.class_set : auto_class();
}

static uint32_t cod_bits(const esp_bt_cod_t *cod) {
    return (uint32_t)cod->service << ESP_BT_COD_SRVC_BIT_OFFSET
           | (uint32_t)cod->major << ESP_BT_COD_MAJOR_DEV_BIT_OFFSET
           | (uint32_t)cod->minor << ESP_BT_COD_MINOR_DEV_BIT_OFFSET;
}

// The class the stack holds now, which is what an inquiry answers with;
// the one wanted while the radio is off.
uint32_t btc_radio_class(void) {
    esp_bt_cod_t cod;
    if (r.on && esp_bt_gap_get_cod(&cod) == ESP_OK) {
        return cod_bits(&cod);
    }
    return wanted_class();
}

static void apply_class(void *arg) {
    (void)arg;
    if (!r.on) {
        return;
    }
    uint32_t c = wanted_class();
    esp_bt_cod_t cod = {0};
    cod.minor = (c & ESP_BT_COD_MINOR_DEV_BIT_MASK) >> ESP_BT_COD_MINOR_DEV_BIT_OFFSET;
    cod.major = (c & ESP_BT_COD_MAJOR_DEV_BIT_MASK) >> ESP_BT_COD_MAJOR_DEV_BIT_OFFSET;
    cod.service = (c & ESP_BT_COD_SRVC_BIT_MASK) >> ESP_BT_COD_SRVC_BIT_OFFSET;
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);
    // The controller answers inquiries with the class it had when inquiry
    // scan was last turned on, so a new class reaches a scanning phone only
    // once scanning restarts. Restart it here, while the board is found.
    bool connectable = r.connectable && !r.hidden;
    if (connectable && r.discoverable && !r.unlisted) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        apply_scan_mode();
    }
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
    // Secure Simple Pairing, by default with no display or keyboard.
    apply_io();
    // Legacy pairing, for devices without SSP.
    apply_pin();
    r.on = true;
    r.hidden = false;
    r.unlisted = false;
    apply_scan_mode();
    schedule_class();
    // Settings made before start, where they differ from the stack's own.
    if (r.page_timeout_ms != 5120) {
        apply_page_timeout();
    }
    if (!channels_default()) {
        apply_channels();
    }
    if (r.min_key_size != BTC_KEY_SIZE_MIN) {
        esp_bt_gap_set_min_enc_key_size((uint8_t)r.min_key_size);
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
    btc_opp_stop();
    btc_pb_stop();
    btc_ag_stop();
    btc_hf_stop();
    btc_sink_stop();
    btc_source_stop();
    btc_spp_stop();
    btc_l2cap_stop();
    btc_hid_stop();
    btc_inquiry_stop();
    r.sdp_state = BTC_ERR_STATE;   // a search the stop cut short
    r.passkey.action = 0;
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

// ---- pairing ----

int btc_radio_set_io(int io) {
    if (io < BTC_IO_DISPLAY_ONLY || io > BTC_IO_NONE) {
        return BTC_ERR_ARG;
    }
    r.io = io;
    if (r.on) {
        apply_io();
    }
    return BTC_OK;
}

int btc_radio_io(void) {
    return r.io;
}

void btc_radio_passkey(btc_passkey_t *out) {
    BTC_LOCK();
    *out = r.passkey;
    BTC_UNLOCK();
}

int btc_radio_passkey_reply(bool accept, uint32_t passkey) {
    BTC_LOCK();
    btc_passkey_t p = r.passkey;
    if (p.action == BTC_PASSKEY_INPUT || p.action == BTC_PASSKEY_COMPARE) {
        r.passkey.action = 0;
    }
    BTC_UNLOCK();
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    esp_bd_addr_t a;
    memcpy(a, p.addr, sizeof(a));
    if (p.action == BTC_PASSKEY_INPUT) {
        return btc_err(esp_bt_gap_ssp_passkey_reply(a, accept, passkey));
    }
    if (p.action == BTC_PASSKEY_COMPARE) {
        return btc_err(esp_bt_gap_ssp_confirm_reply(a, accept));
    }
    return BTC_ERR_STATE;
}

// ---- services ----

int btc_radio_query_services(const uint8_t *addr) {
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    esp_bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    BTC_LOCK();
    if (r.sdp_state == 0) {
        BTC_UNLOCK();
        return BTC_ERR_STATE;
    }
    memcpy(r.sdp_addr, addr, sizeof(btc_addr_t));
    r.sdp_state = 0;
    r.sdp_count = 0;
    BTC_UNLOCK();
    int err = btc_err(esp_bt_gap_get_remote_services(a));
    if (err != BTC_OK) {
        BTC_LOCK();
        r.sdp_state = err;
        BTC_UNLOCK();
    }
    return err;
}

int btc_radio_services(btc_uuid_t *out, int max, int *count) {
    BTC_LOCK();
    int state = r.sdp_state;
    int n = r.sdp_count < max ? r.sdp_count : max;
    if (state == 1) {
        memcpy(out, r.sdp, n * sizeof(btc_uuid_t));
    }
    *count = state == 1 ? n : 0;
    BTC_UNLOCK();
    return state;
}

int btc_radio_set_min_key_size(int bytes) {
    if (bytes < BTC_KEY_SIZE_MIN || bytes > BTC_KEY_SIZE_MAX) {
        return BTC_ERR_ARG;
    }
    r.min_key_size = bytes;
    if (r.on) {
        return btc_err(esp_bt_gap_set_min_enc_key_size((uint8_t)bytes));
    }
    return BTC_OK;
}

int btc_radio_min_key_size(void) {
    return r.min_key_size;
}

static int tune_start(const uint8_t *addr) {
    if (!r.on) {
        return BTC_ERR_STATE;
    }
    BTC_LOCK();
    memcpy(r.tune_addr, addr, sizeof(btc_addr_t));
    r.tune_state = 0;
    BTC_UNLOCK();
    return BTC_OK;
}

static int tune_sent(int err) {
    if (err != BTC_OK) {
        BTC_LOCK();
        r.tune_state = err;
        BTC_UNLOCK();
    }
    return err;
}

int btc_radio_set_poll(const uint8_t *addr, uint32_t ms) {
    if (ms < BTC_POLL_MIN || ms > BTC_POLL_MAX) {
        return BTC_ERR_ARG;
    }
    int err = tune_start(addr);
    if (err != BTC_OK) {
        return err;
    }
    esp_bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    // In slots of 0.625 ms.
    return tune_sent(btc_err(esp_bt_gap_set_qos(a, (ms * 8 + 4) / 5)));
}

int btc_radio_set_packet_types(const uint8_t *addr, uint32_t types) {
    if (types & ~(uint32_t)BTC_PKT_ALL) {
        return BTC_ERR_ARG;
    }
    int err = tune_start(addr);
    if (err != BTC_OK) {
        return err;
    }
    esp_bd_addr_t a;
    memcpy(a, addr, sizeof(a));
    return tune_sent(btc_err(esp_bt_gap_set_acl_pkt_types(a, pkt_to_hci(types))));
}

int btc_radio_tune_result(uint32_t *value) {
    BTC_LOCK();
    int state = r.tune_state;
    *value = r.tune_value;
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
