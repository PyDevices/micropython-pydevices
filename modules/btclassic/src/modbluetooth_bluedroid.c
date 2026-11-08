// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// MicroPython's `bluetooth` module (BLE) on Bluedroid, beside btclassic's
// Bluetooth Classic, so one firmware does both. ESP-IDF runs one Bluetooth
// host, and NimBLE is BLE only; this is the backend extmod/modbluetooth.c
// calls in NimBLE's place.
//
// Bluedroid calls back on its own task, the one that also decodes A2DP
// audio, so nothing here runs Python there: events go through the module's
// ring buffer and Python runs from the scheduler. The GATT server answers
// reads and writes from the same attribute values Python sets with
// gatts_write, copied under the ring buffer's lock, and never allocates on
// Bluedroid's task: a write longer than the value's buffer is refused (set
// a larger one with gatts_set_buffer).
//
// The peripheral and GATT server roles are here. Scanning, the central
// role, the GATT client and pairing are not yet.

#include <string.h>

#include "py/runtime.h"
#include "py/mphal.h"
#include "py/mpthread.h"
#include "py/mperrno.h"
#include "extmod/modbluetooth.h"

#if MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BLUEDROID

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"

#include "btclassic.h"

#define APP_ID 0x4d50
#define MAX_CONN 4
#define MAX_SERVICES 16
#define MAX_CCCD 32
#define MAX_NAME 32
#define WAIT_MS 1000
#define DEFAULT_MTU 256

typedef struct {
    bool used;
    uint16_t conn_id;
    uint8_t addr_type;
    esp_bd_addr_t bda;
    bool indicating;
} conn_t;

typedef struct {
    uint16_t value_handle;
    uint16_t cccd_handle;
    uint16_t sub[MAX_CONN];     // the client's CCCD: bit 0 notify, 1 indicate
} cccd_t;

static struct {
    bool active;
    esp_gatt_if_t gatts_if;
    volatile int8_t reg;        // 0 waiting, 1 done, -1 failed: for each wait below
    volatile int8_t tab;
    volatile int8_t svc;
    volatile int8_t adv_data;
    volatile int8_t adv_sr;
    volatile int8_t adv_start;
    uint16_t tab_handles[64];
    uint16_t tab_n;
    conn_t conn[MAX_CONN];
    uint16_t services[MAX_SERVICES];
    size_t n_services;
    cccd_t cccd[MAX_CCCD];
    size_t n_cccd;
    char name[MAX_NAME + 1];
    size_t name_len;
    uint16_t mtu;
    // One prepared (long) write at a time.
    uint16_t prep_handle;
    uint16_t prep_len;
    uint8_t prep[512];
} b = {.mtu = DEFAULT_MTU};

typedef struct _mp_bluetooth_bluedroid_root_pointers_t {
    mp_gatts_db_t gatts_db;
} mp_bluetooth_bluedroid_root_pointers_t;

MP_REGISTER_ROOT_POINTER(struct _mp_bluetooth_bluedroid_root_pointers_t *bluetooth_bluedroid_root_pointers);

#define DB() (MP_STATE_PORT(bluetooth_bluedroid_root_pointers)->gatts_db)

// Wait for a callback to set *flag, letting other MicroPython threads (and
// a callback that needs the GIL) run meanwhile.
static int wait_for(volatile int8_t *flag) {
    for (int i = 0; i < WAIT_MS / 5 && *flag == 0; i++) {
        MP_THREAD_GIL_EXIT();
        vTaskDelay(pdMS_TO_TICKS(5));
        MP_THREAD_GIL_ENTER();
    }
    return *flag > 0 ? 0 : (*flag == 0 ? MP_ETIMEDOUT : MP_EIO);
}

static conn_t *conn_find(uint16_t conn_id) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (b.conn[i].used && b.conn[i].conn_id == conn_id) {
            return &b.conn[i];
        }
    }
    return NULL;
}

static conn_t *conn_by_bda(const uint8_t *bda) {
    for (int i = 0; i < MAX_CONN; i++) {
        if (b.conn[i].used && memcmp(b.conn[i].bda, bda, 6) == 0) {
            return &b.conn[i];
        }
    }
    return NULL;
}

static cccd_t *cccd_by_handle(uint16_t handle) {
    for (size_t i = 0; i < b.n_cccd; i++) {
        if (b.cccd[i].cccd_handle == handle) {
            return &b.cccd[i];
        }
    }
    return NULL;
}

// ---- the attribute values, shared with Python ----

// Copy up to max bytes of a value from offset; the length, or -1 if the
// handle has no value.
static int db_copy(uint16_t handle, size_t offset, uint8_t *dst, size_t max) {
    int n = -1;
    MICROPY_PY_BLUETOOTH_ENTER
    if (MP_STATE_PORT(bluetooth_bluedroid_root_pointers)) {
        mp_bluetooth_gatts_db_entry_t *e = mp_bluetooth_gatts_db_lookup(DB(), handle);
        if (e) {
            n = 0;
            if (offset < e->data_len) {
                n = e->data_len - offset < max ? e->data_len - offset : max;
                memcpy(dst, e->data + offset, n);
            }
        }
    }
    MICROPY_PY_BLUETOOTH_EXIT
    return n;
}

// Store a client's write without allocating: false if it doesn't fit.
static bool db_store(uint16_t handle, const uint8_t *value, size_t len) {
    bool ok = false;
    MICROPY_PY_BLUETOOTH_ENTER
    if (MP_STATE_PORT(bluetooth_bluedroid_root_pointers)) {
        mp_bluetooth_gatts_db_entry_t *e = mp_bluetooth_gatts_db_lookup(DB(), handle);
        if (e) {
            size_t start = e->append ? e->data_len : 0;
            if (start + len <= e->data_alloc) {
                memcpy(e->data + start, value, len);
                e->data_len = start + len;
                ok = true;
            }
        }
    }
    MICROPY_PY_BLUETOOTH_EXIT
    return ok;
}

// ---- callbacks, on Bluedroid's task ----

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            b.adv_data = param->adv_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            b.adv_sr = param->scan_rsp_data_raw_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            b.adv_start = param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT: {
            conn_t *c = conn_by_bda(param->update_conn_params.bda);
            if (c) {
                mp_bluetooth_gap_on_connection_update(c->conn_id, param->update_conn_params.conn_int,
                    param->update_conn_params.latency, param->update_conn_params.timeout,
                    param->update_conn_params.status == ESP_BT_STATUS_SUCCESS ? 0 : param->update_conn_params.status);
            }
            break;
        }
        default:
            break;
    }
}

static void respond(uint16_t conn_id, uint32_t trans_id, uint16_t handle, esp_gatt_status_t status,
    const uint8_t *value, size_t len, uint16_t offset) {
    static esp_gatt_rsp_t rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.attr_value.handle = handle;
    rsp.attr_value.offset = offset;
    rsp.attr_value.len = len;
    if (len) {
        memcpy(rsp.attr_value.value, value, len);
    }
    esp_ble_gatts_send_response(b.gatts_if, conn_id, trans_id, status, &rsp);
}

static void on_read(struct gatts_read_evt_param *rd) {
    if (!rd->need_rsp) {
        return;
    }
    conn_t *c = conn_find(rd->conn_id);
    cccd_t *cc = cccd_by_handle(rd->handle);
    static uint8_t buf[ESP_GATT_MAX_ATTR_LEN];
    if (cc) {
        uint16_t v = c ? cc->sub[c - b.conn] : 0;
        buf[0] = v & 0xff;
        buf[1] = v >> 8;
        respond(rd->conn_id, rd->trans_id, rd->handle, ESP_GATT_OK, buf + rd->offset, rd->offset < 2 ? 2 - rd->offset : 0, rd->offset);
        return;
    }
    size_t max = b.mtu > 1 ? b.mtu - 1 : 22;
    if (max > sizeof(buf)) {
        max = sizeof(buf);
    }
    int n = db_copy(rd->handle, rd->offset, buf, max);
    respond(rd->conn_id, rd->trans_id, rd->handle, n < 0 ? ESP_GATT_INVALID_HANDLE : ESP_GATT_OK,
        buf, n < 0 ? 0 : n, rd->offset);
}

static void on_write(struct gatts_write_evt_param *wr) {
    esp_gatt_status_t status = ESP_GATT_OK;
    if (wr->is_prep) {
        if (b.prep_handle && b.prep_handle != wr->handle) {
            status = ESP_GATT_PREPARE_Q_FULL;
        } else if (wr->offset != b.prep_len || wr->offset + wr->len > sizeof(b.prep)) {
            status = ESP_GATT_INVALID_OFFSET;
        } else {
            b.prep_handle = wr->handle;
            memcpy(b.prep + wr->offset, wr->value, wr->len);
            b.prep_len = wr->offset + wr->len;
        }
        if (wr->need_rsp) {
            respond(wr->conn_id, wr->trans_id, wr->handle, status, wr->value, wr->len, wr->offset);
        }
        return;
    }
    cccd_t *cc = cccd_by_handle(wr->handle);
    conn_t *c = conn_find(wr->conn_id);
    if (cc) {
        if (c && wr->len >= 2) {
            cc->sub[c - b.conn] = wr->value[0] | (wr->value[1] << 8);
        }
    } else if (wr->offset != 0) {
        status = ESP_GATT_INVALID_OFFSET;
    } else if (!db_store(wr->handle, wr->value, wr->len)) {
        status = ESP_GATT_INVALID_ATTR_LEN;
    }
    if (wr->need_rsp) {
        respond(wr->conn_id, wr->trans_id, wr->handle, status, NULL, 0, 0);
    }
    if (!cc && status == ESP_GATT_OK) {
        mp_bluetooth_gatts_on_write(wr->conn_id, wr->handle);
    }
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param) {
    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.app_id == APP_ID) {
                b.gatts_if = gatts_if;
                b.reg = param->reg.status == ESP_GATT_OK ? 1 : -1;
            }
            return;
        default:
            break;
    }
    if (gatts_if != b.gatts_if) {
        return;
    }
    switch (event) {
        case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
            uint16_t n = param->add_attr_tab.num_handle;
            if (n > MP_ARRAY_SIZE(b.tab_handles)) {
                n = MP_ARRAY_SIZE(b.tab_handles);
            }
            memcpy(b.tab_handles, param->add_attr_tab.handles, n * sizeof(uint16_t));
            b.tab_n = n;
            b.tab = param->add_attr_tab.status == ESP_GATT_OK ? 1 : -1;
            break;
        }
        case ESP_GATTS_START_EVT:
            b.svc = param->start.status == ESP_GATT_OK ? 1 : -1;
            break;
        case ESP_GATTS_DELETE_EVT:
            b.svc = param->del.status == ESP_GATT_OK ? 1 : -1;
            break;
        case ESP_GATTS_CONNECT_EVT: {
            for (int i = 0; i < MAX_CONN; i++) {
                if (!b.conn[i].used) {
                    conn_t *c = &b.conn[i];
                    c->used = true;
                    c->conn_id = param->connect.conn_id;
                    c->addr_type = param->connect.ble_addr_type;
                    c->indicating = false;
                    memcpy(c->bda, param->connect.remote_bda, 6);
                    for (size_t k = 0; k < b.n_cccd; k++) {
                        b.cccd[k].sub[i] = 0;
                    }
                    break;
                }
            }
            mp_bluetooth_gap_on_connected_disconnected(MP_BLUETOOTH_IRQ_CENTRAL_CONNECT,
                param->connect.conn_id, param->connect.ble_addr_type, param->connect.remote_bda);
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT: {
            conn_t *c = conn_find(param->disconnect.conn_id);
            uint8_t type = c ? c->addr_type : 0;
            if (c) {
                c->used = false;
            }
            mp_bluetooth_gap_on_connected_disconnected(MP_BLUETOOTH_IRQ_CENTRAL_DISCONNECT,
                param->disconnect.conn_id, type, param->disconnect.remote_bda);
            break;
        }
        case ESP_GATTS_MTU_EVT:
            mp_bluetooth_gatts_on_mtu_exchanged(param->mtu.conn_id, param->mtu.mtu);
            break;
        case ESP_GATTS_READ_EVT:
            on_read(&param->read);
            break;
        case ESP_GATTS_WRITE_EVT:
            on_write(&param->write);
            break;
        case ESP_GATTS_EXEC_WRITE_EVT: {
            esp_gatt_status_t status = ESP_GATT_OK;
            uint16_t handle = b.prep_handle;
            if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && handle) {
                if (!db_store(handle, b.prep, b.prep_len)) {
                    status = ESP_GATT_INVALID_ATTR_LEN;
                }
            }
            b.prep_handle = 0;
            b.prep_len = 0;
            respond(param->exec_write.conn_id, param->exec_write.trans_id, handle, status, NULL, 0, 0);
            if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && handle && status == ESP_GATT_OK) {
                mp_bluetooth_gatts_on_write(param->exec_write.conn_id, handle);
            }
            break;
        }
        case ESP_GATTS_CONF_EVT: {
            conn_t *c = conn_find(param->conf.conn_id);
            if (c && c->indicating) {
                c->indicating = false;
                mp_bluetooth_gatts_on_indicate_complete(param->conf.conn_id, param->conf.handle,
                    param->conf.status == ESP_GATT_OK ? 0 : param->conf.status);
            }
            break;
        }
        default:
            break;
    }
}

// ---- on and off ----

int mp_bluetooth_init(void) {
    if (b.active) {
        return 0;
    }
    MP_STATE_PORT(bluetooth_bluedroid_root_pointers) = m_new0(mp_bluetooth_bluedroid_root_pointers_t, 1);
    mp_bluetooth_gatts_db_create(&MP_STATE_PORT(bluetooth_bluedroid_root_pointers)->gatts_db);
    if (btc_stack_ble_start() != BTC_OK) {
        MP_STATE_PORT(bluetooth_bluedroid_root_pointers) = NULL;
        return MP_EIO;
    }
    memset(b.conn, 0, sizeof(b.conn));
    b.n_services = 0;
    b.n_cccd = 0;
    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gatts_register_callback(gatts_cb);
    b.reg = 0;
    b.gatts_if = ESP_GATT_IF_NONE;
    int err = esp_ble_gatts_app_register(APP_ID) == ESP_OK ? wait_for(&b.reg) : MP_EIO;
    if (err) {
        btc_stack_ble_stop();
        MP_STATE_PORT(bluetooth_bluedroid_root_pointers) = NULL;
        return err;
    }
    esp_ble_gatt_set_local_mtu(b.mtu);
    if (b.name_len) {
        esp_ble_gap_set_device_name(b.name);
    }
    b.active = true;
    return 0;
}

int mp_bluetooth_deinit(void) {
    if (!b.active) {
        return 0;
    }
    esp_ble_gap_stop_advertising();
    for (int i = 0; i < MAX_CONN; i++) {
        if (b.conn[i].used) {
            esp_ble_gap_disconnect(b.conn[i].bda);
        }
    }
    // Unregistering the app removes its services.
    esp_ble_gatts_app_unregister(b.gatts_if);
    b.gatts_if = ESP_GATT_IF_NONE;
    b.active = false;
    memset(b.conn, 0, sizeof(b.conn));
    b.n_services = 0;
    b.n_cccd = 0;
    btc_stack_ble_stop();
    MP_STATE_PORT(bluetooth_bluedroid_root_pointers) = NULL;
    return 0;
}

bool mp_bluetooth_is_active(void) {
    return b.active;
}

void mp_bluetooth_get_current_address(uint8_t *addr_type, uint8_t *addr) {
    // The public address, which Classic shares.
    *addr_type = BLE_ADDR_TYPE_PUBLIC;
    const uint8_t *a = b.active ? esp_bt_dev_get_address() : NULL;
    if (a) {
        memcpy(addr, a, 6);
    } else {
        memset(addr, 0, 6);
    }
}

void mp_bluetooth_set_address_mode(uint8_t addr_mode) {
    // Only the public address so far.
    (void)addr_mode;
}

size_t mp_bluetooth_gap_get_device_name(const uint8_t **buf) {
    *buf = (const uint8_t *)b.name;
    return b.name_len;
}

int mp_bluetooth_gap_set_device_name(const uint8_t *buf, size_t len) {
    if (len > MAX_NAME) {
        return MP_EINVAL;
    }
    memcpy(b.name, buf, len);
    b.name[len] = 0;
    b.name_len = len;
    if (b.active) {
        esp_ble_gap_set_device_name(b.name);
    }
    return 0;
}

int mp_bluetooth_get_preferred_mtu(void) {
    return b.mtu;
}

int mp_bluetooth_set_preferred_mtu(uint16_t mtu) {
    if (mtu < 23 || mtu > ESP_GATT_MAX_MTU_SIZE) {
        return MP_EINVAL;
    }
    b.mtu = mtu;
    if (b.active) {
        esp_ble_gatt_set_local_mtu(mtu);
    }
    return 0;
}

// ---- advertising ----

int mp_bluetooth_gap_advertise_start(bool connectable, int32_t interval_us, const uint8_t *adv_data, size_t adv_data_len, const uint8_t *sr_data, size_t sr_data_len) {
    if (!b.active) {
        return MP_ENODEV;
    }
    esp_ble_gap_stop_advertising();
    int err = 0;
    if (adv_data) {
        b.adv_data = 0;
        err = esp_ble_gap_config_adv_data_raw((uint8_t *)adv_data, adv_data_len) == ESP_OK ? wait_for(&b.adv_data) : MP_EINVAL;
    }
    if (!err && sr_data) {
        b.adv_sr = 0;
        err = esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)sr_data, sr_data_len) == ESP_OK ? wait_for(&b.adv_sr) : MP_EINVAL;
    }
    if (err) {
        return err;
    }
    int32_t units = interval_us / 625;
    if (units < 0x20) {
        units = 0x20;
    } else if (units > 0x4000) {
        units = 0x4000;
    }
    esp_ble_adv_params_t p = {
        .adv_int_min = units,
        .adv_int_max = units,
        .adv_type = connectable ? ADV_TYPE_IND : (sr_data_len ? ADV_TYPE_SCAN_IND : ADV_TYPE_NONCONN_IND),
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .channel_map = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    b.adv_start = 0;
    return esp_ble_gap_start_advertising(&p) == ESP_OK ? wait_for(&b.adv_start) : MP_EINVAL;
}

void mp_bluetooth_gap_advertise_stop(void) {
    if (b.active) {
        esp_ble_gap_stop_advertising();
    }
}

int mp_bluetooth_gap_disconnect(uint16_t conn_handle) {
    if (!b.active) {
        return MP_ENODEV;
    }
    conn_t *c = conn_find(conn_handle);
    if (!c) {
        return MP_ENOTCONN;
    }
    return esp_ble_gap_disconnect(c->bda) == ESP_OK ? 0 : MP_EIO;
}

// ---- the GATT server ----

static const uint16_t uuid_primary = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t uuid_char_decl = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t uuid_cccd = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

// Base UUID 0000xxxx-0000-1000-8000-00805F9B34FB, little-endian, for
// turning a 32-bit UUID into the 128-bit form GATT carries.
static const uint8_t base_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0
};

static void put_uuid(esp_attr_desc_t *d, const mp_obj_bluetooth_uuid_t *u, uint8_t *scratch128) {
    if (u->type == MP_BLUETOOTH_UUID_TYPE_32) {
        memcpy(scratch128, base_uuid, 16);
        memcpy(scratch128 + 12, u->data, 4);
        d->uuid_length = ESP_UUID_LEN_128;
        d->uuid_p = scratch128;
    } else {
        d->uuid_length = u->type;
        d->uuid_p = (uint8_t *)u->data;
    }
}

static uint16_t perm_of(uint16_t flags) {
    uint16_t p = 0;
    if (flags & MP_BLUETOOTH_CHARACTERISTIC_FLAG_READ) {
        p |= ESP_GATT_PERM_READ;
    }
    if (flags & MP_BLUETOOTH_CHARACTERISTIC_FLAG_READ_ENCRYPTED) {
        p |= ESP_GATT_PERM_READ_ENCRYPTED;
    }
    if (flags & MP_BLUETOOTH_CHARACTERISTIC_FLAG_READ_AUTHENTICATED) {
        p |= ESP_GATT_PERM_READ_ENC_MITM;
    }
    if (flags & (MP_BLUETOOTH_CHARACTERISTIC_FLAG_WRITE | MP_BLUETOOTH_CHARACTERISTIC_FLAG_WRITE_NO_RESPONSE)) {
        p |= ESP_GATT_PERM_WRITE;
    }
    if (flags & MP_BLUETOOTH_CHARACTERISTIC_FLAG_WRITE_ENCRYPTED) {
        p |= ESP_GATT_PERM_WRITE_ENCRYPTED;
    }
    if (flags & MP_BLUETOOTH_CHARACTERISTIC_FLAG_WRITE_AUTHENTICATED) {
        p |= ESP_GATT_PERM_WRITE_ENC_MITM;
    }
    return p;
}

int mp_bluetooth_gatts_register_service_begin(bool append) {
    if (!b.active) {
        return MP_ENODEV;
    }
    if (append) {
        return 0;
    }
    for (size_t i = 0; i < b.n_services; i++) {
        b.svc = 0;
        if (esp_ble_gatts_delete_service(b.services[i]) == ESP_OK) {
            wait_for(&b.svc);
        }
    }
    b.n_services = 0;
    b.n_cccd = 0;
    mp_bluetooth_gatts_db_reset(DB());
    return 0;
}

int mp_bluetooth_gatts_register_service(mp_obj_bluetooth_uuid_t *service_uuid, mp_obj_bluetooth_uuid_t **characteristic_uuids, uint16_t *characteristic_flags, mp_obj_bluetooth_uuid_t **descriptor_uuids, uint16_t *descriptor_flags, uint8_t *num_descriptors, uint16_t *handles, size_t num_characteristics) {
    if (!b.active) {
        return MP_ENODEV;
    }
    if (b.n_services >= MAX_SERVICES) {
        return MP_ENOMEM;
    }
    // The table: the service, then for each characteristic its declaration,
    // its value, a CCCD if it notifies or indicates, and its descriptors.
    size_t n = 1, n_desc = 0;
    for (size_t i = 0; i < num_characteristics; i++) {
        bool cccd = characteristic_flags[i] & (MP_BLUETOOTH_CHARACTERISTIC_FLAG_NOTIFY | MP_BLUETOOTH_CHARACTERISTIC_FLAG_INDICATE);
        n += 2 + (cccd ? 1 : 0) + num_descriptors[i];
        n_desc += num_descriptors[i];
    }
    if (n > MP_ARRAY_SIZE(b.tab_handles)) {
        return MP_E2BIG;
    }
    // Everything the table points at lives until the stack has built it:
    // the entries, the declarations' property bytes and 128-bit UUIDs.
    esp_gatts_attr_db_t *tab = m_new0(esp_gatts_attr_db_t, n);
    uint8_t *props = m_new0(uint8_t, num_characteristics);
    uint8_t *u128 = m_new0(uint8_t, 16 * (n + 1));
    uint8_t *svc_value = u128 + 16 * n;
    size_t k = 0;

    tab[k].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    tab[k].att_desc.uuid_length = ESP_UUID_LEN_16;
    tab[k].att_desc.uuid_p = (uint8_t *)&uuid_primary;
    tab[k].att_desc.perm = ESP_GATT_PERM_READ;
    if (service_uuid->type == MP_BLUETOOTH_UUID_TYPE_32) {
        memcpy(svc_value, base_uuid, 16);
        memcpy(svc_value + 12, service_uuid->data, 4);
        tab[k].att_desc.length = tab[k].att_desc.max_length = 16;
    } else {
        memcpy(svc_value, service_uuid->data, service_uuid->type);
        tab[k].att_desc.length = tab[k].att_desc.max_length = service_uuid->type;
    }
    tab[k].att_desc.value = svc_value;
    k++;

    size_t d = 0;
    for (size_t i = 0; i < num_characteristics; i++) {
        uint16_t f = characteristic_flags[i];
        props[i] = f & 0xff;
        tab[k].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        tab[k].att_desc.uuid_length = ESP_UUID_LEN_16;
        tab[k].att_desc.uuid_p = (uint8_t *)&uuid_char_decl;
        tab[k].att_desc.perm = ESP_GATT_PERM_READ;
        tab[k].att_desc.length = tab[k].att_desc.max_length = 1;
        tab[k].att_desc.value = &props[i];
        k++;
        tab[k].attr_control.auto_rsp = ESP_GATT_RSP_BY_APP;
        put_uuid(&tab[k].att_desc, characteristic_uuids[i], u128 + 16 * k);
        tab[k].att_desc.perm = perm_of(f);
        tab[k].att_desc.max_length = ESP_GATT_MAX_ATTR_LEN;
        k++;
        if (f & (MP_BLUETOOTH_CHARACTERISTIC_FLAG_NOTIFY | MP_BLUETOOTH_CHARACTERISTIC_FLAG_INDICATE)) {
            tab[k].attr_control.auto_rsp = ESP_GATT_RSP_BY_APP;
            tab[k].att_desc.uuid_length = ESP_UUID_LEN_16;
            tab[k].att_desc.uuid_p = (uint8_t *)&uuid_cccd;
            tab[k].att_desc.perm = ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE;
            tab[k].att_desc.max_length = 2;
            k++;
        }
        for (size_t j = 0; j < num_descriptors[i]; j++, d++) {
            tab[k].attr_control.auto_rsp = ESP_GATT_RSP_BY_APP;
            put_uuid(&tab[k].att_desc, descriptor_uuids[d], u128 + 16 * k);
            tab[k].att_desc.perm = perm_of(descriptor_flags[d]);
            tab[k].att_desc.max_length = ESP_GATT_MAX_ATTR_LEN;
            k++;
        }
    }

    b.tab = 0;
    int err = esp_ble_gatts_create_attr_tab(tab, b.gatts_if, n, b.n_services) == ESP_OK ? wait_for(&b.tab) : MP_EINVAL;
    m_del(esp_gatts_attr_db_t, tab, n);
    m_del(uint8_t, props, num_characteristics);
    m_del(uint8_t, u128, 16 * (n + 1));
    if (err) {
        return err;
    }
    if (b.tab_n != n) {
        return MP_EIO;
    }

    // Hand back each value's handle followed by its descriptors', as the
    // module expects; the CCCDs are ours.
    k = 1;
    size_t h = 0;
    for (size_t i = 0; i < num_characteristics; i++) {
        uint16_t f = characteristic_flags[i];
        uint16_t value = b.tab_handles[k + 1];
        k += 2;
        handles[h++] = value;
        mp_bluetooth_gatts_db_create_entry(DB(), value, MP_BLUETOOTH_DEFAULT_ATTR_LEN);
        if (f & (MP_BLUETOOTH_CHARACTERISTIC_FLAG_NOTIFY | MP_BLUETOOTH_CHARACTERISTIC_FLAG_INDICATE)) {
            if (b.n_cccd < MAX_CCCD) {
                cccd_t *cc = &b.cccd[b.n_cccd++];
                memset(cc, 0, sizeof(*cc));
                cc->value_handle = value;
                cc->cccd_handle = b.tab_handles[k];
            }
            k++;
        }
        for (size_t j = 0; j < num_descriptors[i]; j++) {
            handles[h++] = b.tab_handles[k];
            mp_bluetooth_gatts_db_create_entry(DB(), b.tab_handles[k], MP_BLUETOOTH_DEFAULT_ATTR_LEN);
            k++;
        }
    }
    (void)n_desc;

    b.svc = 0;
    err = esp_ble_gatts_start_service(b.tab_handles[0]) == ESP_OK ? wait_for(&b.svc) : MP_EIO;
    b.services[b.n_services++] = b.tab_handles[0];
    return err;
}

int mp_bluetooth_gatts_register_service_end(void) {
    return 0;
}

int mp_bluetooth_gatts_read(uint16_t value_handle, const uint8_t **value, size_t *value_len) {
    if (!b.active) {
        return MP_ENODEV;
    }
    return mp_bluetooth_gatts_db_read(DB(), value_handle, value, value_len);
}

static int send(conn_t *c, uint16_t value_handle, bool indicate, const uint8_t *value, size_t len) {
    if (indicate) {
        c->indicating = true;
    }
    esp_err_t e = esp_ble_gatts_send_indicate(b.gatts_if, c->conn_id, value_handle, len, (uint8_t *)value, indicate);
    if (e != ESP_OK) {
        c->indicating = false;
        return MP_EIO;
    }
    return 0;
}

int mp_bluetooth_gatts_write(uint16_t value_handle, const uint8_t *value, size_t value_len, bool send_update) {
    if (!b.active) {
        return MP_ENODEV;
    }
    int err = mp_bluetooth_gatts_db_write(DB(), value_handle, value, value_len);
    if (err || !send_update) {
        return err;
    }
    for (size_t i = 0; i < b.n_cccd; i++) {
        if (b.cccd[i].value_handle != value_handle) {
            continue;
        }
        for (int j = 0; j < MAX_CONN; j++) {
            uint16_t sub = b.cccd[i].sub[j];
            if (b.conn[j].used && sub) {
                send(&b.conn[j], value_handle, !(sub & 1), value, value_len);
            }
        }
    }
    return 0;
}

int mp_bluetooth_gatts_notify_indicate(uint16_t conn_handle, uint16_t value_handle, int gatts_op, const uint8_t *value, size_t value_len) {
    if (!b.active) {
        return MP_ENODEV;
    }
    conn_t *c = conn_find(conn_handle);
    if (!c) {
        return MP_ENOTCONN;
    }
    if (!value) {
        int err = mp_bluetooth_gatts_db_read(DB(), value_handle, &value, &value_len);
        if (err) {
            return err;
        }
    }
    return send(c, value_handle, gatts_op == MP_BLUETOOTH_GATTS_OP_INDICATE, value, value_len);
}

int mp_bluetooth_gatts_set_buffer(uint16_t value_handle, size_t len, bool append) {
    if (!b.active) {
        return MP_ENODEV;
    }
    return mp_bluetooth_gatts_db_resize(DB(), value_handle, len, append);
}

#endif // MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BLUEDROID
