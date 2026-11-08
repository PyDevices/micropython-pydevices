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
// Peripheral and central, the GATT server and client, scanning, and the
// four address modes are here. Pairing is not yet.
//
// A connection's handle is Bluedroid's connection ID, which is the same
// number for the GATT server and the GATT client on one link, so one handle
// serves both roles as it does upstream.

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
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "btclassic.h"

#define APP_ID 0x4d50
#define MAX_CONN 4
#define MAX_SERVICES 16
#define MAX_CCCD 32
#define MAX_NAME 32
#define WAIT_MS 1000
#define DEFAULT_MTU 256

#define WRITE_FIFO 16

typedef struct {
    bool used;
    bool central;               // the peer is the central (it connected to us)
    bool client;                // our GATT client has the link open
    uint16_t conn_id;
    uint8_t addr_type;
    esp_bd_addr_t bda;
    bool indicating;
    // Bluedroid answers every client write, with or without response, in
    // the order they were sent; this says which ones Python hears about.
    uint8_t wr_mode[WRITE_FIFO];
    uint8_t wr_head;
    uint8_t wr_count;
} conn_t;

typedef struct {
    uint16_t value_handle;
    uint16_t cccd_handle;
    uint16_t sub[MAX_CONN];     // the client's CCCD: bit 0 notify, 1 indicate
} cccd_t;

static struct {
    bool active;
    esp_gatt_if_t gatts_if;
    esp_gatt_if_t gattc_if;
    volatile int8_t reg;        // 0 waiting, 1 done, -1 failed: for each wait below
    volatile int8_t creg;
    volatile int8_t open;
    volatile int8_t scan_param;
    volatile int8_t scan_start;
    volatile int8_t scan_stop;
    volatile int8_t rand_addr;
    volatile int8_t privacy;
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
    // Our address: its mode (MP_BLUETOOTH_ADDRESS_MODE_*), the type the
    // controller sends, and the random one when there is one.
    uint8_t addr_mode;
    esp_ble_addr_type_t own_type;
    esp_bd_addr_t rand;
    // Scanning, and one outgoing connection at a time.
    volatile bool scanning;
    volatile bool connecting;
    esp_bd_addr_t connecting_bda;
    uint8_t connecting_type;
    esp_timer_handle_t scan_timer;
    esp_timer_handle_t conn_timer;
    // One prepared (long) write at a time.
    uint16_t prep_handle;
    uint16_t prep_len;
    uint8_t prep[512];
} b = {.mtu = DEFAULT_MTU, .own_type = BLE_ADDR_TYPE_PUBLIC};

// Bluedroid's own cancel for a pending connection: ESP-IDF's API has none.
// It posts a message to Bluedroid's task, as the API calls do.
extern void BTA_GATTC_CancelOpen(uint8_t client_if, uint8_t *remote_bda, bool is_direct);

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

// The entry for a link, made on first sight: the server's and the client's
// events for a new link can come in either order.
static conn_t *conn_add(uint16_t conn_id, const uint8_t *bda, uint8_t addr_type) {
    conn_t *c = conn_find(conn_id);
    if (c) {
        return c;
    }
    for (int i = 0; i < MAX_CONN; i++) {
        if (!b.conn[i].used) {
            c = &b.conn[i];
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->conn_id = conn_id;
            c->addr_type = addr_type;
            memcpy(c->bda, bda, 6);
            for (size_t k = 0; k < b.n_cccd; k++) {
                b.cccd[k].sub[i] = 0;
            }
            return c;
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
        case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:
            b.rand_addr = param->set_rand_addr_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_SET_LOCAL_PRIVACY_COMPLETE_EVT:
            b.privacy = param->local_privacy_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            b.scan_param = param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            b.scan_start = param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            b.scan_stop = param->scan_stop_cmpl.status == ESP_BT_STATUS_SUCCESS ? 1 : -1;
            if (b.scanning) {
                b.scanning = false;
                mp_bluetooth_gap_on_scan_complete();
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT: {
            struct ble_scan_result_evt_param *r = &param->scan_rst;
            if (r->search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                // An advertisement and a scan response come apart, each
                // with its own type (the numbers are upstream's adv_type).
                const uint8_t *data = r->ble_adv;
                uint16_t len = r->adv_data_len;
                if (r->ble_evt_type == ESP_BLE_EVT_SCAN_RSP) {
                    data += r->adv_data_len;
                    len = r->scan_rsp_len;
                }
                mp_bluetooth_gap_on_scan_result(r->ble_addr_type, r->bda, r->ble_evt_type, r->rssi, data, len);
            } else if (r->search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT && b.scanning) {
                b.scanning = false;
                mp_bluetooth_gap_on_scan_complete();
            }
            break;
        }
        #endif
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
            // Every link reaches the server, whichever side opened it, so
            // this is where Python hears of each connection, once.
            bool central = param->connect.link_role == 1;
            conn_t *c = conn_add(param->connect.conn_id, param->connect.remote_bda, param->connect.ble_addr_type);
            if (c) {
                c->central = central;
                c->addr_type = param->connect.ble_addr_type;
            }
            if (!central && b.connecting && memcmp(b.connecting_bda, param->connect.remote_bda, 6) == 0) {
                b.connecting = false;
                esp_timer_stop(b.conn_timer);
            }
            mp_bluetooth_gap_on_connected_disconnected(central ? MP_BLUETOOTH_IRQ_CENTRAL_CONNECT : MP_BLUETOOTH_IRQ_PERIPHERAL_CONNECT,
                param->connect.conn_id, param->connect.ble_addr_type, param->connect.remote_bda);
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT: {
            conn_t *c = conn_find(param->disconnect.conn_id);
            uint8_t type = c ? c->addr_type : 0;
            bool central = c ? c->central : true;
            if (c) {
                c->used = false;
            }
            mp_bluetooth_gap_on_connected_disconnected(central ? MP_BLUETOOTH_IRQ_CENTRAL_DISCONNECT : MP_BLUETOOTH_IRQ_PERIPHERAL_DISCONNECT,
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

#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT

// A pending connection that failed or was cancelled: upstream reports it as
// a disconnect with handle 0xffff.
static void connect_failed(const uint8_t *bda, uint8_t addr_type) {
    mp_bluetooth_gap_on_connected_disconnected(MP_BLUETOOTH_IRQ_PERIPHERAL_DISCONNECT, 0xffff, addr_type, bda);
}

static void uuid_from_esp(mp_obj_bluetooth_uuid_t *u, const esp_bt_uuid_t *e) {
    u->base.type = &mp_type_bluetooth_uuid;
    u->type = e->len;
    memcpy(u->data, &e->uuid, e->len);
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id == APP_ID) {
            b.gattc_if = gattc_if;
            b.creg = param->reg.status == ESP_GATT_OK ? 1 : -1;
        }
        return;
    }
    if (gattc_if != b.gattc_if) {
        return;
    }
    switch (event) {
        case ESP_GATTC_OPEN_EVT:
            if (param->open.status == ESP_GATT_OK) {
                conn_t *c = conn_add(param->open.conn_id, param->open.remote_bda, b.connecting_type);
                if (c) {
                    c->client = true;
                }
                b.open = 1;
            } else {
                b.open = -1;
                if (b.connecting && memcmp(b.connecting_bda, param->open.remote_bda, 6) == 0) {
                    b.connecting = false;
                    esp_timer_stop(b.conn_timer);
                    connect_failed(param->open.remote_bda, b.connecting_type);
                }
            }
            break;
        case ESP_GATTC_CLOSE_EVT:
        case ESP_GATTC_DISCONNECT_EVT: {
            uint16_t id = event == ESP_GATTC_CLOSE_EVT ? param->close.conn_id : param->disconnect.conn_id;
            conn_t *c = conn_find(id);
            if (c) {
                c->client = false;
                c->wr_count = 0;
            }
            break;
        }
        case ESP_GATTC_CFG_MTU_EVT:
            if (param->cfg_mtu.status == ESP_GATT_OK) {
                mp_bluetooth_gatts_on_mtu_exchanged(param->cfg_mtu.conn_id, param->cfg_mtu.mtu);
            }
            break;
        case ESP_GATTC_SEARCH_RES_EVT:
            if (param->search_res.is_primary) {
                mp_obj_bluetooth_uuid_t u;
                uuid_from_esp(&u, &param->search_res.srvc_id.uuid);
                mp_bluetooth_gattc_on_primary_service_result(param->search_res.conn_id,
                    param->search_res.start_handle, param->search_res.end_handle, &u);
            }
            break;
        case ESP_GATTC_SEARCH_CMPL_EVT:
            mp_bluetooth_gattc_on_discover_complete(MP_BLUETOOTH_IRQ_GATTC_SERVICE_DONE,
                param->search_cmpl.conn_id, param->search_cmpl.status);
            break;
        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT: {
            struct gattc_read_char_evt_param *rd = &param->read;
            if (rd->status == ESP_GATT_OK) {
                const uint8_t *v = rd->value;
                uint16_t n = rd->value_len;
                mp_bluetooth_gattc_on_data_available(MP_BLUETOOTH_IRQ_GATTC_READ_RESULT, rd->conn_id, rd->handle, &v, &n, 1);
            }
            mp_bluetooth_gattc_on_read_write_status(MP_BLUETOOTH_IRQ_GATTC_READ_DONE, rd->conn_id, rd->handle, rd->status);
            break;
        }
        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT: {
            conn_t *c = conn_find(param->write.conn_id);
            uint8_t mode = MP_BLUETOOTH_WRITE_MODE_WITH_RESPONSE;
            MICROPY_PY_BLUETOOTH_ENTER
            if (c && c->wr_count) {
                mode = c->wr_mode[c->wr_head];
                c->wr_head = (c->wr_head + 1) % WRITE_FIFO;
                c->wr_count--;
            }
            MICROPY_PY_BLUETOOTH_EXIT
            if (mode == MP_BLUETOOTH_WRITE_MODE_WITH_RESPONSE) {
                mp_bluetooth_gattc_on_read_write_status(MP_BLUETOOTH_IRQ_GATTC_WRITE_DONE,
                    param->write.conn_id, param->write.handle, param->write.status);
            }
            break;
        }
        case ESP_GATTC_NOTIFY_EVT: {
            // Bluedroid confirms an indication itself, after this.
            const uint8_t *v = param->notify.value;
            uint16_t n = param->notify.value_len;
            mp_bluetooth_gattc_on_data_available(param->notify.is_notify ? MP_BLUETOOTH_IRQ_GATTC_NOTIFY : MP_BLUETOOTH_IRQ_GATTC_INDICATE,
                param->notify.conn_id, param->notify.handle, &v, &n, 1);
            break;
        }
        default:
            break;
    }
}

#endif // MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT

// ---- on and off ----

#if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
// Timers, on the esp_timer task: end a timed scan, give up a connection.
static void scan_timeout(void *arg) {
    if (b.scanning) {
        esp_ble_gap_stop_scanning();
    }
}

static void connect_timeout(void *arg) {
    if (b.connecting) {
        b.connecting = false;
        BTA_GATTC_CancelOpen(b.gattc_if, b.connecting_bda, true);
        connect_failed(b.connecting_bda, b.connecting_type);
    }
}
#endif

static int timers_make(void) {
    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    if (!b.scan_timer) {
        const esp_timer_create_args_t a = {.callback = scan_timeout, .name = "ble_scan"};
        if (esp_timer_create(&a, &b.scan_timer) != ESP_OK) {
            return MP_ENOMEM;
        }
    }
    if (!b.conn_timer) {
        const esp_timer_create_args_t a = {.callback = connect_timeout, .name = "ble_conn"};
        if (esp_timer_create(&a, &b.conn_timer) != ESP_OK) {
            return MP_ENOMEM;
        }
    }
    #endif
    return 0;
}

int mp_bluetooth_init(void) {
    if (b.active) {
        return 0;
    }
    int err = timers_make();
    if (err) {
        return err;
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
    b.scanning = false;
    b.connecting = false;
    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gatts_register_callback(gatts_cb);
    b.reg = 0;
    b.gatts_if = ESP_GATT_IF_NONE;
    err = esp_ble_gatts_app_register(APP_ID) == ESP_OK ? wait_for(&b.reg) : MP_EIO;
    #if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT
    if (!err) {
        esp_ble_gattc_register_callback(gattc_cb);
        b.creg = 0;
        b.gattc_if = ESP_GATT_IF_NONE;
        err = esp_ble_gattc_app_register(APP_ID) == ESP_OK ? wait_for(&b.creg) : MP_EIO;
        if (err && b.gatts_if != ESP_GATT_IF_NONE) {
            esp_ble_gatts_app_unregister(b.gatts_if);
        }
    }
    #endif
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
    // Back to the public address each time, as upstream does.
    b.addr_mode = MP_BLUETOOTH_ADDRESS_MODE_PUBLIC;
    b.own_type = BLE_ADDR_TYPE_PUBLIC;
    return 0;
}

int mp_bluetooth_deinit(void) {
    if (!b.active) {
        return 0;
    }
    esp_ble_gap_stop_advertising();
    #if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE
    esp_timer_stop(b.scan_timer);
    esp_timer_stop(b.conn_timer);
    if (b.scanning) {
        b.scanning = false;
        esp_ble_gap_stop_scanning();
    }
    if (b.connecting) {
        b.connecting = false;
        BTA_GATTC_CancelOpen(b.gattc_if, b.connecting_bda, true);
    }
    #endif
    for (int i = 0; i < MAX_CONN; i++) {
        if (b.conn[i].used) {
            esp_ble_gap_disconnect(b.conn[i].bda);
        }
    }
    if (b.addr_mode == MP_BLUETOOTH_ADDRESS_MODE_RPA) {
        esp_ble_gap_config_local_privacy(false);
    }
    // Unregistering the apps removes the services and the client's links.
    #if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT
    esp_ble_gattc_app_unregister(b.gattc_if);
    b.gattc_if = ESP_GATT_IF_NONE;
    #endif
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
    const uint8_t *a = NULL;
    if (b.own_type == BLE_ADDR_TYPE_RANDOM) {
        *addr_type = BLE_ADDR_TYPE_RANDOM;
        a = b.rand;
    } else {
        // The public address, which Classic shares. With privacy on, the
        // controller makes the resolvable address and Bluedroid doesn't
        // say which one is current, so this is the identity address.
        *addr_type = BLE_ADDR_TYPE_PUBLIC;
        a = b.active ? esp_bt_dev_get_address() : NULL;
    }
    if (a) {
        memcpy(addr, a, 6);
    } else {
        memset(addr, 0, 6);
    }
}

// A random address: static (top two bits 11) or non-resolvable private
// (top two bits 00). The address is written most significant byte first.
static int set_random(bool static_addr) {
    esp_bd_addr_t a;
    do {
        esp_fill_random(a, 6);
        if (static_addr) {
            a[0] |= 0xc0;
        } else {
            a[0] &= 0x3f;
        }
        // Neither all ones nor all zeros below the top two bits.
    } while ((a[0] & 0x3f) == 0 && !a[1] && !a[2] && !a[3] && !a[4] && !a[5]);
    b.rand_addr = 0;
    int err = esp_ble_gap_set_rand_addr(a) == ESP_OK ? wait_for(&b.rand_addr) : MP_EINVAL;
    if (!err) {
        memcpy(b.rand, a, 6);
    }
    return err;
}

void mp_bluetooth_set_address_mode(uint8_t addr_mode) {
    if (!b.active) {
        mp_raise_OSError(MP_ENODEV);
    }
    int err = 0;
    if (b.addr_mode == MP_BLUETOOTH_ADDRESS_MODE_RPA && addr_mode != MP_BLUETOOTH_ADDRESS_MODE_RPA) {
        b.privacy = 0;
        err = esp_ble_gap_config_local_privacy(false) == ESP_OK ? wait_for(&b.privacy) : MP_EIO;
    }
    switch (addr_mode) {
        case MP_BLUETOOTH_ADDRESS_MODE_PUBLIC:
            b.own_type = BLE_ADDR_TYPE_PUBLIC;
            break;
        case MP_BLUETOOTH_ADDRESS_MODE_RANDOM:
            // A new static address each time BLE starts, as upstream's
            // NimBLE port does.
            if (!err && b.addr_mode != MP_BLUETOOTH_ADDRESS_MODE_RANDOM) {
                err = set_random(true);
            }
            b.own_type = BLE_ADDR_TYPE_RANDOM;
            break;
        case MP_BLUETOOTH_ADDRESS_MODE_RPA:
            // The controller makes and rotates the address from our
            // identity key (Bluedroid keeps it); peers that bonded with us
            // can resolve it.
            if (!err && b.addr_mode != MP_BLUETOOTH_ADDRESS_MODE_RPA) {
                b.privacy = 0;
                err = esp_ble_gap_config_local_privacy(true) == ESP_OK ? wait_for(&b.privacy) : MP_EOPNOTSUPP;
            }
            b.own_type = BLE_ADDR_TYPE_RPA_PUBLIC;
            break;
        case MP_BLUETOOTH_ADDRESS_MODE_NRPA:
            if (!err) {
                err = set_random(false);
            }
            b.own_type = BLE_ADDR_TYPE_RANDOM;
            break;
        default:
            err = MP_EINVAL;
            break;
    }
    if (err) {
        b.own_type = BLE_ADDR_TYPE_PUBLIC;
        b.addr_mode = MP_BLUETOOTH_ADDRESS_MODE_PUBLIC;
        mp_raise_OSError(err);
    }
    b.addr_mode = addr_mode;
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
        .own_addr_type = b.own_type,
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

#if MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE

// ---- scanning and the central role ----

static uint16_t units_625(int32_t us, uint16_t lo, uint16_t hi) {
    int32_t n = us / 625;
    return n < lo ? lo : (n > hi ? hi : n);
}

int mp_bluetooth_gap_scan_start(int32_t duration_ms, int32_t interval_us, int32_t window_us, bool active_scan) {
    if (!b.active) {
        return MP_ENODEV;
    }
    if (b.scanning) {
        return MP_EALREADY;
    }
    uint16_t interval = units_625(interval_us, 0x4, 0x4000);
    uint16_t window = units_625(window_us, 0x4, 0x4000);
    esp_ble_scan_params_t p = {
        .scan_type = active_scan ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = b.own_type,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = interval,
        .scan_window = window < interval ? window : interval,
        // Every advertisement, as upstream reports them.
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    b.scan_param = 0;
    int err = esp_ble_gap_set_scan_params(&p) == ESP_OK ? wait_for(&b.scan_param) : MP_EINVAL;
    if (err) {
        return err;
    }
    // Bluedroid times scans in whole seconds; a timer ends it to the
    // millisecond instead, and 0 scans until stopped.
    b.scanning = true;
    b.scan_start = 0;
    err = esp_ble_gap_start_scanning(0) == ESP_OK ? wait_for(&b.scan_start) : MP_EIO;
    if (err) {
        b.scanning = false;
        return err;
    }
    if (duration_ms > 0) {
        esp_timer_start_once(b.scan_timer, (uint64_t)duration_ms * 1000);
    }
    return 0;
}

int mp_bluetooth_gap_scan_stop(void) {
    if (!b.active) {
        return MP_ENODEV;
    }
    esp_timer_stop(b.scan_timer);
    if (!b.scanning) {
        return 0;
    }
    b.scan_stop = 0;
    return esp_ble_gap_stop_scanning() == ESP_OK ? wait_for(&b.scan_stop) : MP_EIO;
}

int mp_bluetooth_gap_peripheral_connect(uint8_t addr_type, const uint8_t *addr, int32_t duration_ms, int32_t min_conn_interval_us, int32_t max_conn_interval_us) {
    if (!b.active) {
        return MP_ENODEV;
    }
    if (b.connecting) {
        return MP_EALREADY;
    }
    if (b.scanning) {
        // The controller can't scan while it opens a connection here.
        mp_bluetooth_gap_scan_stop();
    }
    esp_ble_conn_params_t cp = {
        .scan_interval = 0x10,
        .scan_window = 0x10,
        .latency = 0,
        .supervision_timeout = 400,     // 4 s
    };
    esp_ble_gatt_creat_conn_params_t p = {
        .remote_addr_type = addr_type,
        .is_direct = true,
        .is_aux = false,
        .own_addr_type = b.own_type,
    };
    memcpy(p.remote_bda, addr, 6);
    if (min_conn_interval_us > 0 || max_conn_interval_us > 0) {
        int32_t lo = (min_conn_interval_us > 0 ? min_conn_interval_us : max_conn_interval_us) / 1250;
        int32_t hi = (max_conn_interval_us > 0 ? max_conn_interval_us : min_conn_interval_us) / 1250;
        cp.interval_min = lo < 6 ? 6 : (lo > 0xc80 ? 0xc80 : lo);
        cp.interval_max = hi < cp.interval_min ? cp.interval_min : (hi > 0xc80 ? 0xc80 : hi);
        p.phy_mask = ESP_BLE_PHY_1M_PREF_MASK;
        p.phy_1m_conn_params = &cp;
    }
    memcpy(b.connecting_bda, addr, 6);
    b.connecting_type = addr_type;
    b.connecting = true;
    if (esp_ble_gattc_enh_open(b.gattc_if, &p) != ESP_OK) {
        b.connecting = false;
        return MP_EIO;
    }
    if (duration_ms > 0) {
        esp_timer_start_once(b.conn_timer, (uint64_t)duration_ms * 1000);
    }
    return 0;
}

int mp_bluetooth_gap_peripheral_connect_cancel(void) {
    if (!b.active) {
        return MP_ENODEV;
    }
    esp_timer_stop(b.conn_timer);
    connect_timeout(NULL);
    return 0;
}

#endif // MICROPY_PY_BLUETOOTH_ENABLE_CENTRAL_MODE

#if MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT

// ---- the GATT client ----

// The link's entry with our client open on it. A link the peer opened
// (we're its peripheral) gets the client added on first use.
static int client_conn(uint16_t conn_handle, conn_t **out) {
    if (!b.active) {
        return MP_ENODEV;
    }
    conn_t *c = conn_find(conn_handle);
    if (!c) {
        return MP_ENOTCONN;
    }
    if (!c->client) {
        b.open = 0;
        if (esp_ble_gattc_open(b.gattc_if, c->bda, c->addr_type, true) != ESP_OK) {
            return MP_EIO;
        }
        int err = wait_for(&b.open);
        if (err) {
            return err;
        }
        if (!c->client) {
            return MP_ENOTCONN;
        }
    }
    *out = c;
    return 0;
}

int mp_bluetooth_gattc_discover_primary_services(uint16_t conn_handle, const mp_obj_bluetooth_uuid_t *uuid) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    esp_bt_uuid_t f;
    if (uuid) {
        f.len = uuid->type;
        memcpy(&f.uuid, uuid->data, uuid->type);
    }
    // Bluedroid discovers the whole database the first time and keeps it;
    // characteristics and descriptors are then read from that copy.
    return esp_ble_gattc_search_service(b.gattc_if, conn_handle, uuid ? &f : NULL) == ESP_OK ? 0 : MP_EIO;
}

static bool uuid_eq(const esp_bt_uuid_t *e, const mp_obj_bluetooth_uuid_t *u) {
    return e->len == u->type && memcmp(&e->uuid, u->data, u->type) == 0;
}

#define DB_CHUNK 24

// Walk the attributes Bluedroid found from start to end, a chunk at a
// time, calling fn on each. Runs on the MicroPython thread.
typedef void (*db_fn_t)(uint16_t conn, const esp_gattc_db_elem_t *e, void *ctx);

static int db_walk(uint16_t conn, uint16_t start, uint16_t end, db_fn_t fn, void *ctx) {
    esp_gattc_db_elem_t *db = m_new(esp_gattc_db_elem_t, DB_CHUNK);
    while (start <= end && start != 0) {
        uint16_t n = DB_CHUNK;
        esp_gatt_status_t st = esp_ble_gattc_get_db(b.gattc_if, conn, start, end, db, &n);
        if (st != ESP_GATT_OK || n == 0) {
            break;
        }
        uint16_t last = start;
        for (uint16_t i = 0; i < n; i++) {
            fn(conn, &db[i], ctx);
            if (db[i].attribute_handle > last) {
                last = db[i].attribute_handle;
            }
        }
        if (n < DB_CHUNK || last == 0xffff) {
            break;
        }
        start = last + 1;
    }
    m_del(esp_gattc_db_elem_t, db, DB_CHUNK);
    return 0;
}

typedef struct {
    const mp_obj_bluetooth_uuid_t *filter;
    uint16_t end;           // the end of the range asked for
    uint16_t svc_end;       // the end of the service we're in, when known
    bool have;              // a characteristic waiting for its end handle
    esp_gattc_db_elem_t pending;
    conn_t *c;
} chr_walk_t;

static void chr_emit(uint16_t conn, chr_walk_t *w, uint16_t end) {
    esp_gattc_db_elem_t *e = &w->pending;
    w->have = false;
    if (w->filter && !uuid_eq(&e->uuid, w->filter)) {
        return;
    }
    if (e->properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE)) {
        // Bluedroid hands on notifications only for handles registered
        // here (up to CONFIG_BT_GATTC_NOTIF_REG_MAX for each peer).
        esp_ble_gattc_register_for_notify(b.gattc_if, w->c->bda, e->attribute_handle);
    }
    mp_obj_bluetooth_uuid_t u;
    uuid_from_esp(&u, &e->uuid);
    mp_bluetooth_gattc_on_characteristic_result(conn, e->attribute_handle, end, e->properties, &u);
}

static void chr_fn(uint16_t conn, const esp_gattc_db_elem_t *e, void *ctx) {
    chr_walk_t *w = ctx;
    uint16_t boundary;
    switch (e->type) {
        case ESP_GATT_DB_PRIMARY_SERVICE:
        case ESP_GATT_DB_SECONDARY_SERVICE:
            boundary = e->start_handle;
            break;
        case ESP_GATT_DB_CHARACTERISTIC:
            boundary = e->attribute_handle - 1;     // its declaration
            break;
        default:
            return;
    }
    if (w->have) {
        uint16_t end = boundary - 1;
        chr_emit(conn, w, w->svc_end && w->svc_end < end ? w->svc_end : end);
    }
    if (e->type == ESP_GATT_DB_CHARACTERISTIC) {
        w->pending = *e;
        w->have = true;
    } else {
        w->svc_end = e->end_handle;
    }
}

int mp_bluetooth_gattc_discover_characteristics(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle, const mp_obj_bluetooth_uuid_t *uuid) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    chr_walk_t w = {.filter = uuid, .end = end_handle, .c = c};
    db_walk(conn_handle, start_handle, end_handle, chr_fn, &w);
    if (w.have) {
        chr_emit(conn_handle, &w, w.svc_end && w.svc_end < end_handle ? w.svc_end : end_handle);
    }
    mp_bluetooth_gattc_on_discover_complete(MP_BLUETOOTH_IRQ_GATTC_CHARACTERISTIC_DONE, conn_handle, 0);
    return 0;
}

static void dsc_fn(uint16_t conn, const esp_gattc_db_elem_t *e, void *ctx) {
    if (e->type == ESP_GATT_DB_DESCRIPTOR) {
        mp_obj_bluetooth_uuid_t u;
        uuid_from_esp(&u, &e->uuid);
        mp_bluetooth_gattc_on_descriptor_result(conn, e->attribute_handle, &u);
    }
}

int mp_bluetooth_gattc_discover_descriptors(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    db_walk(conn_handle, start_handle, end_handle, dsc_fn, NULL);
    mp_bluetooth_gattc_on_discover_complete(MP_BLUETOOTH_IRQ_GATTC_DESCRIPTOR_DONE, conn_handle, 0);
    return 0;
}

int mp_bluetooth_gattc_read(uint16_t conn_handle, uint16_t value_handle) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    esp_err_t e = esp_ble_gattc_read_char(b.gattc_if, conn_handle, value_handle, ESP_GATT_AUTH_REQ_NONE);
    return e == ESP_OK ? 0 : (e == ESP_FAIL ? MP_EBUSY : MP_EIO);
}

int mp_bluetooth_gattc_write(uint16_t conn_handle, uint16_t value_handle, const uint8_t *value, size_t value_len, unsigned int mode) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    if (value_len == 2 && (value[0] & 3) && !(value[0] & ~3) && value[1] == 0 && value_handle > 1) {
        // Looks like a client turning on notifications at a CCCD whose
        // characteristic it never discovered: register its value handle.
        esp_ble_gattc_register_for_notify(b.gattc_if, c->bda, value_handle - 1);
    }
    // Note the mode first: the answer can come back before this returns.
    bool full;
    MICROPY_PY_BLUETOOTH_ENTER
    full = c->wr_count == WRITE_FIFO;
    if (!full) {
        c->wr_mode[(c->wr_head + c->wr_count) % WRITE_FIFO] = mode;
        c->wr_count++;
    }
    MICROPY_PY_BLUETOOTH_EXIT
    if (full) {
        return MP_EBUSY;
    }
    esp_err_t e = esp_ble_gattc_write_char(b.gattc_if, conn_handle, value_handle, value_len, (uint8_t *)value,
        mode == MP_BLUETOOTH_WRITE_MODE_WITH_RESPONSE ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE);
    if (e != ESP_OK) {
        MICROPY_PY_BLUETOOTH_ENTER
        if (c->wr_count) {
            c->wr_count--;
        }
        MICROPY_PY_BLUETOOTH_EXIT
        return e == ESP_FAIL ? MP_EBUSY : MP_EIO;
    }
    return 0;
}

int mp_bluetooth_gattc_exchange_mtu(uint16_t conn_handle) {
    conn_t *c;
    int err = client_conn(conn_handle, &c);
    if (err) {
        return err;
    }
    return esp_ble_gattc_send_mtu_req(b.gattc_if, conn_handle) == ESP_OK ? 0 : MP_EIO;
}

#endif // MICROPY_PY_BLUETOOTH_ENABLE_GATT_CLIENT

#endif // MICROPY_PY_BLUETOOTH && MICROPY_BLUETOOTH_BLUEDROID
