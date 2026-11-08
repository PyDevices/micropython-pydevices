// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// HID over Bluetooth Classic on Bluedroid, one role at a time: the board as
// a device (a keyboard, mouse or gamepad a PC or phone pairs with), or as a
// host (it pairs with a Bluetooth keyboard, mouse or gamepad and reads its
// reports). See include/btclassic.h.
//
// Reports cross btclassic.h as the descriptor lays them out, report ID
// first when it has IDs. Bluedroid's device side takes the ID apart from
// the data, so this file splits and joins it; its host side hands reports
// over whole. Bluedroid sends one report at a time and drops one that finds
// the link congested, so the device keeps a queue and sends the next once
// ESP_HIDD_SEND_REPORT_EVT says the last went, retrying a congested one.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "btc_bluedroid.h"

#if CONFIG_BT_HID_DEVICE_ENABLED
#include "esp_hidd_api.h"
#endif
#if CONFIG_BT_HID_HOST_ENABLED
#include "esp_hidh_api.h"
#endif

#if CONFIG_BT_HID_DEVICE_ENABLED || CONFIG_BT_HID_HOST_ENABLED

static const char *TAG = "btclassic";

#define QUEUE_BYTES 512         // reports waiting, each with a length byte
#define MAX_REPORT 64
#define HOST_DESCRIPTOR_MAX 884 // BTHH_MAX_DSC_LEN
#define LAST_INPUTS 4           // input reports kept to answer GET_REPORT
#define CONGESTED 8             // HID_ERR_CONGESTED, from Bluedroid's hiddefs.h
#define RETRY_US 5000

typedef struct {
    uint8_t buf[QUEUE_BYTES];
    uint16_t head, count;
} queue_t;

static struct {
    bool started;
    bool host;              // the role
    bool connected;
    bool boot;              // the link runs the boot protocol
    bool ready;             // the stack finished starting the role
    bool stopping;
    btc_addr_t peer;
    uint32_t sent, received, dropped;
    uint32_t changes;
    queue_t q;              // device: reports to send; host: reports received

    // device
    bool has_ids;           // the descriptor has report IDs
    bool in_flight;         // a report is with Bluedroid
    uint8_t flight[MAX_REPORT];
    uint8_t flight_len;
    uint8_t output[MAX_REPORT + 1];     // the last report the host set, ID first
    uint8_t output_len;
    struct {
        uint8_t len;        // 0: unused
        uint8_t report[MAX_REPORT];
    } last_input[LAST_INPUTS];

    // host
    bool want_boot;
    uint16_t descriptor_len;
} h;

// The last peer, for connect() with none; kept across stop and start.
static bool have_last;
static btc_addr_t last;

// Device: the descriptor and the service record's texts, held while the
// role runs.
static uint8_t *descriptor;
static size_t descriptor_len;
static char service_name[64];
static esp_timer_handle_t retry_timer;

// Host: the device's descriptor, from SDP.
static uint8_t *host_descriptor;

static void mark(uint32_t bits) {
    BTC_LOCK();
    h.changes |= bits;
    BTC_UNLOCK();
}

// ---- the queue: length-prefixed reports, under the lock ----

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

// The last peer of each role, kept in NVS so connect() can call it back
// after a reset: Bluedroid doesn't keep the device role's host itself.
#define NVS_NAMESPACE "btclassic"

static const char *nvs_key(void) {
    return h.host ? "hid_device" : "hid_host";
}

static void remember_last(const uint8_t *addr) {
    nvs_handle_t n;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &n) == ESP_OK) {
        nvs_set_blob(n, nvs_key(), addr, 6);
        nvs_commit(n);
        nvs_close(n);
    }
}

static void recall_last(void) {
    nvs_handle_t n;
    uint8_t a[6];
    size_t len = sizeof(a);
    if (have_last || nvs_open(NVS_NAMESPACE, NVS_READONLY, &n) != ESP_OK) {
        return;
    }
    if (nvs_get_blob(n, nvs_key(), a, &len) == ESP_OK && len == 6) {
        memcpy(last, a, 6);
        have_last = true;
    }
    nvs_close(n);
}

static void link_up(const uint8_t *addr) {
    BTC_LOCK();
    memcpy(h.peer, addr, 6);
    memcpy(last, addr, 6);
    have_last = true;
    h.connected = true;
    h.boot = false;
    h.in_flight = false;
    h.changes |= BTC_CHANGED_CONNECTION;
    BTC_UNLOCK();
    remember_last(addr);
    ESP_LOGI(TAG, "hid: %02x:%02x:%02x:%02x:%02x:%02x connected", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static void link_down(void) {
    BTC_LOCK();
    bool was = h.connected;
    h.connected = false;
    h.boot = false;
    h.in_flight = false;
    h.q.head = h.q.count = 0;
    h.descriptor_len = 0;
    if (was) {
        h.changes |= BTC_CHANGED_CONNECTION;
    }
    BTC_UNLOCK();
    if (was) {
        ESP_LOGI(TAG, "hid: closed");
    }
}

// Whether a report descriptor has a Report ID item (0x85), walking its
// items rather than its bytes.
static bool descriptor_has_ids(const uint8_t *d, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t prefix = d[i];
        if (prefix == 0xfe) {   // long item: size in the next byte
            if (i + 1 >= len) {
                break;
            }
            i += 3 + d[i + 1];
            continue;
        }
        static const uint8_t sizes[4] = {0, 1, 2, 4};
        if ((prefix & 0xfc) == 0x84) {
            return true;
        }
        i += 1 + sizes[prefix & 3];
    }
    return false;
}

// =========================================================================
// device
// =========================================================================

#if CONFIG_BT_HID_DEVICE_ENABLED

static esp_hidd_app_param_t app_param;
static esp_hidd_qos_param_t qos;    // zero: no QoS asked

// Send the next queued report if none is with Bluedroid. Any task.
static void device_kick(void) {
    uint8_t msg[MAX_REPORT];
    size_t n = 0;
    BTC_LOCK();
    if (h.started && !h.host && h.connected && !h.in_flight) {
        if (h.flight_len == 0) {
            h.flight_len = (uint8_t)queue_get(h.flight, sizeof(h.flight));
        }
        if (h.flight_len) {
            n = h.flight_len;
            memcpy(msg, h.flight, n);
            h.in_flight = true;
        }
    }
    bool ids = h.has_ids || h.boot;
    BTC_UNLOCK();
    if (n == 0) {
        return;
    }
    // Keep it to answer the host's GET_REPORT.
    uint8_t id = ids ? msg[0] : 0;
    BTC_LOCK();
    int slot = -1;
    for (int i = 0; i < LAST_INPUTS; i++) {
        if (h.last_input[i].len && (!ids || h.last_input[i].report[0] == id)) {
            slot = i;
            break;
        }
        if (slot < 0 && h.last_input[i].len == 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        h.last_input[slot].len = (uint8_t)n;
        memcpy(h.last_input[slot].report, msg, n);
    }
    BTC_UNLOCK();
    esp_err_t err = ids
        ? esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, id, (uint16_t)(n - 1), msg + 1)
        : esp_bt_hid_device_send_report(ESP_HIDD_REPORT_TYPE_INTRDATA, 0, (uint16_t)n, msg);
    if (err != ESP_OK) {
        BTC_LOCK();
        h.in_flight = false;
        h.flight_len = 0;
        h.dropped++;
        BTC_UNLOCK();
    }
}

static void retry_cb(void *arg) {
    (void)arg;
    device_kick();
}

// The host asks for a report: the last input report with that ID, or the
// last output report.
static void answer_get_report(esp_hidd_report_type_t type, uint8_t id) {
    uint8_t r[MAX_REPORT + 1];
    size_t n = 0;
    BTC_LOCK();
    bool ids = h.has_ids || h.boot;
    if (type == ESP_HIDD_REPORT_TYPE_INPUT) {
        for (int i = 0; i < LAST_INPUTS; i++) {
            if (h.last_input[i].len && (!ids || h.last_input[i].report[0] == id)) {
                n = h.last_input[i].len;
                memcpy(r, h.last_input[i].report, n);
                break;
            }
        }
    } else if (type == ESP_HIDD_REPORT_TYPE_OUTPUT && h.output_len && (!ids || h.output[0] == id)) {
        n = h.output_len;
        memcpy(r, h.output, n);
    }
    BTC_UNLOCK();
    if (n == 0 || (ids && n < 2)) {
        esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_INVALID_REP_ID);
        return;
    }
    // On the control channel; see ESP_HIDD_SEND_REPORT_EVT below.
    if (ids) {
        esp_bt_hid_device_send_report(type, id, (uint16_t)(n - 1), r + 1);
    } else {
        esp_bt_hid_device_send_report(type, 0, (uint16_t)n, r);
    }
}

static void store_output(uint8_t id, const uint8_t *data, uint16_t len) {
    BTC_LOCK();
    bool ids = h.has_ids || h.boot;
    size_t n = 0;
    if (ids) {
        h.output[n++] = id;
    }
    if (len > MAX_REPORT) {
        len = MAX_REPORT;
    }
    memcpy(h.output + n, data, len);
    h.output_len = (uint8_t)(n + len);
    h.changes |= BTC_CHANGED_HID;
    BTC_UNLOCK();
}

static void hidd_cb(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param) {
    switch (event) {
        case ESP_HIDD_INIT_EVT:
            if (param->init.status == ESP_HIDD_SUCCESS) {
                esp_bt_hid_device_register_app(&app_param, &qos, &qos);
            } else {
                ESP_LOGW(TAG, "hid: device init failed: %d", param->init.status);
                h.stopping = true;     // start() sees it
            }
            break;
        case ESP_HIDD_REGISTER_APP_EVT:
            if (param->register_app.status == ESP_HIDD_SUCCESS) {
                if (param->register_app.in_use) {
                    // The host it was plugged into last.
                    memcpy(last, param->register_app.bd_addr, 6);
                    have_last = true;
                }
                h.ready = true;
            } else {
                ESP_LOGW(TAG, "hid: register failed: %d", param->register_app.status);
                h.stopping = true;
            }
            break;
        case ESP_HIDD_UNREGISTER_APP_EVT:
        case ESP_HIDD_DEINIT_EVT:
            h.ready = false;
            break;
        case ESP_HIDD_OPEN_EVT:
            if (param->open.status == ESP_HIDD_SUCCESS && param->open.conn_status == ESP_HIDD_CONN_STATE_CONNECTED) {
                link_up(param->open.bd_addr);
                device_kick();
            } else if (param->open.status != ESP_HIDD_SUCCESS) {
                ESP_LOGW(TAG, "hid: connect failed: %d", param->open.status);
                mark(BTC_CHANGED_CONNECTION);
            }
            break;
        case ESP_HIDD_CLOSE_EVT:
            if (param->close.conn_status == ESP_HIDD_CONN_STATE_DISCONNECTED) {
                link_down();
            }
            break;
        case ESP_HIDD_VC_UNPLUG_EVT:
            link_down();
            break;
        case ESP_HIDD_SEND_REPORT_EVT: {
            // Interrupt reports come back as INPUT. So does a GET_REPORT
            // answer for an input report, which the next line can't tell
            // apart: if one lands while a report is in flight, that report
            // counts as sent early, and a retry of the one after it can be
            // missed. Hosts ask for reports rarely, so that costs at most
            // one report, rarely.
            if (param->send_report.report_type != ESP_HIDD_REPORT_TYPE_INPUT) {
                break;
            }
            bool retry = false;
            BTC_LOCK();
            if (h.in_flight) {
                h.in_flight = false;
                if (param->send_report.status == ESP_HIDD_SUCCESS) {
                    h.sent++;
                    h.flight_len = 0;
                } else if (param->send_report.reason == CONGESTED && h.connected) {
                    retry = true;   // keep it in flight_len and send it again
                } else {
                    h.dropped++;
                    h.flight_len = 0;
                }
                if (h.q.count == 0 && h.flight_len == 0) {
                    h.changes |= BTC_CHANGED_HID;   // room again
                }
            }
            BTC_UNLOCK();
            if (retry) {
                esp_timer_start_once(retry_timer, RETRY_US);
            } else {
                device_kick();
            }
            break;
        }
        case ESP_HIDD_GET_REPORT_EVT:
            answer_get_report(param->get_report.report_type, param->get_report.report_id);
            break;
        case ESP_HIDD_SET_REPORT_EVT:
            if (param->set_report.report_type == ESP_HIDD_REPORT_TYPE_OUTPUT) {
                store_output(param->set_report.report_id, param->set_report.data, param->set_report.len);
                esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_SUCCESS);
            } else {
                esp_bt_hid_device_report_error(ESP_HID_PAR_HANDSHAKE_RSP_ERR_UNSUPPORTED_REQ);
            }
            break;
        case ESP_HIDD_INTR_DATA_EVT:
            // An output report on the interrupt channel: a keyboard's LEDs.
            store_output(param->intr_data.report_id, param->intr_data.data, param->intr_data.len);
            break;
        case ESP_HIDD_SET_PROTOCOL_EVT:
            BTC_LOCK();
            h.boot = param->set_protocol.protocol_mode == ESP_HIDD_BOOT_MODE;
            h.changes |= BTC_CHANGED_HID;
            BTC_UNLOCK();
            break;
        default:
            break;
    }
}

int btc_hid_device_start(const uint8_t *desc, size_t len, uint8_t subclass, const char *name, bool boot) {
    (void)boot;     // Bluedroid answers SET_PROTOCOL for any descriptor
    if (h.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (desc == NULL || len == 0 || len > HOST_DESCRIPTOR_MAX) {
        return BTC_ERR_ARG;
    }
    if (retry_timer == NULL) {
        esp_timer_create_args_t targs = {.callback = retry_cb, .name = "btc_hid"};
        if (esp_timer_create(&targs, &retry_timer) != ESP_OK) {
            return BTC_ERR_NOMEM;
        }
    }
    descriptor = malloc(len);
    if (descriptor == NULL) {
        return BTC_ERR_NOMEM;
    }
    memcpy(descriptor, desc, len);
    descriptor_len = len;
    memset(&h, 0, sizeof(h));
    have_last = false;
    recall_last();
    h.has_ids = descriptor_has_ids(desc, len);
    strncpy(service_name, name && *name ? name : "Keyboard", sizeof(service_name) - 1);
    service_name[sizeof(service_name) - 1] = 0;
    if (subclass == 0) {
        subclass = 0x40;
    }
    app_param = (esp_hidd_app_param_t){
        .name = service_name,
        .description = service_name,
        .provider = "btclassic",
        .subclass = subclass,
        .desc_list = descriptor,
        .desc_list_len = (int)len,
    };

    h.host = false;
    h.started = true;
    esp_err_t err = esp_bt_hid_device_register_callback(hidd_cb);
    if (err == ESP_OK) {
        err = esp_bt_hid_device_init();
    }
    // Wait for the stack to register the service, up to a second.
    for (int i = 0; err == ESP_OK && i < 100 && !h.ready && !h.stopping; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK || !h.ready) {
        ESP_LOGW(TAG, "hid: device didn't start: %s", esp_err_to_name(err));
        btc_hid_stop();
        return err != ESP_OK ? btc_err(err) : BTC_ERR_FAIL;
    }
    // A computer shows the icon and offers the device by its Class of
    // Device: Peripheral, and what kind, unless audio runs too.
    btc_radio_profile(BTC_PROFILE_HID_DEVICE, true, subclass);
    return BTC_OK;
}

int btc_hid_device_connect(const uint8_t *addr) {
    if (!h.started || h.host) {
        return BTC_ERR_STATE;
    }
    if (h.connected) {
        return BTC_OK;
    }
    esp_bd_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else if (have_last) {
        memcpy(a, last, 6);
    } else {
        return BTC_ERR_ARG;
    }
    return btc_err(esp_bt_hid_device_connect(a));
}

int btc_hid_device_send(const uint8_t *report, size_t len) {
    if (!h.started || h.host) {
        return BTC_ERR_STATE;
    }
    if (len == 0 || len > MAX_REPORT) {
        return BTC_ERR_ARG;
    }
    BTC_LOCK();
    bool ok = h.connected && queue_put(report, len);
    BTC_UNLOCK();
    if (!ok) {
        return 0;   // full: try again after the queue drains; or no host
    }
    device_kick();
    return 1;
}

size_t btc_hid_device_output(uint8_t *dst, size_t size) {
    BTC_LOCK();
    size_t n = h.output_len < size ? h.output_len : size;
    memcpy(dst, h.output, n);
    BTC_UNLOCK();
    return n;
}

static void device_stop(void) {
    if (h.connected) {
        esp_bt_hid_device_disconnect();
        for (int i = 0; i < 50 && h.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (retry_timer) {
        esp_timer_stop(retry_timer);
    }
    h.ready = true;
    if (esp_bt_hid_device_unregister_app() == ESP_OK) {
        for (int i = 0; i < 100 && h.ready; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    h.ready = true;
    if (esp_bt_hid_device_deinit() == ESP_OK) {
        for (int i = 0; i < 100 && h.ready; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    btc_radio_profile(BTC_PROFILE_HID_DEVICE, false, 0);
    free(descriptor);
    descriptor = NULL;
    descriptor_len = 0;
}

#else

int btc_hid_device_start(const uint8_t *desc, size_t len, uint8_t subclass, const char *name, bool boot) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_device_connect(const uint8_t *addr) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_device_send(const uint8_t *report, size_t len) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_device_output(uint8_t *dst, size_t size) {
    return 0;
}
static void device_stop(void) {
}

#endif // CONFIG_BT_HID_DEVICE_ENABLED

// =========================================================================
// host
// =========================================================================

#if CONFIG_BT_HID_HOST_ENABLED

static void hidh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param) {
    switch (event) {
        case ESP_HIDH_INIT_EVT:
            if (param->init.status == ESP_HIDH_OK) {
                h.ready = true;
            } else {
                ESP_LOGW(TAG, "hid: host init failed: %d", param->init.status);
                h.stopping = true;
            }
            break;
        case ESP_HIDH_DEINIT_EVT:
            h.ready = false;
            break;
        case ESP_HIDH_OPEN_EVT:
            if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED) {
                if (h.connected && memcmp(h.peer, param->open.bd_addr, 6) != 0) {
                    // One device at a time.
                    esp_bt_hid_host_disconnect(param->open.bd_addr);
                    break;
                }
                link_up(param->open.bd_addr);
                if (h.want_boot) {
                    esp_bt_hid_host_set_protocol(param->open.bd_addr, ESP_HIDH_BOOT_MODE);
                }
            } else if (param->open.status != ESP_HIDH_OK) {
                ESP_LOGW(TAG, "hid: connect failed: %d", param->open.status);
                mark(BTC_CHANGED_CONNECTION);
            }
            break;
        case ESP_HIDH_CLOSE_EVT:
            if (param->close.conn_status == ESP_HIDH_CONN_STATE_DISCONNECTED) {
                link_down();
            }
            break;
        case ESP_HIDH_VC_UNPLUG_EVT:
            link_down();
            break;
        case ESP_HIDH_GET_DSCP_EVT:
            if (param->dscp.status == ESP_HIDH_OK && host_descriptor && param->dscp.dsc_list) {
                size_t n = param->dscp.dl_len < HOST_DESCRIPTOR_MAX ? param->dscp.dl_len : HOST_DESCRIPTOR_MAX;
                BTC_LOCK();
                memcpy(host_descriptor, param->dscp.dsc_list, n);
                h.descriptor_len = (uint16_t)n;
                h.changes |= BTC_CHANGED_HID;
                BTC_UNLOCK();
                ESP_LOGI(TAG, "hid: descriptor %u bytes, vendor %04x product %04x", (unsigned)n,
                    param->dscp.vendor_id, param->dscp.product_id);
            }
            break;
        case ESP_HIDH_SET_PROTO_EVT:
            if (param->set_proto.status == ESP_HIDH_OK) {
                BTC_LOCK();
                h.boot = h.want_boot;
                h.changes |= BTC_CHANGED_HID;
                BTC_UNLOCK();
            } else {
                ESP_LOGW(TAG, "hid: the device refused the boot protocol: %d", param->set_proto.status);
            }
            break;
        case ESP_HIDH_DATA_IND_EVT:
            if (param->data_ind.status != ESP_HIDH_OK || param->data_ind.data == NULL) {
                break;
            }
            BTC_LOCK();
            h.received++;
            if (!queue_put(param->data_ind.data, param->data_ind.len)) {
                h.dropped++;
            }
            h.changes |= BTC_CHANGED_HID;
            BTC_UNLOCK();
            break;
        case ESP_HIDH_DATA_EVT:
            BTC_LOCK();
            if (param->send_data.status == ESP_HIDH_OK) {
                h.sent++;
            } else {
                h.dropped++;
            }
            BTC_UNLOCK();
            break;
        default:
            break;
    }
}

int btc_hid_host_start(void) {
    if (h.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (host_descriptor == NULL) {
        host_descriptor = heap_caps_malloc(HOST_DESCRIPTOR_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (host_descriptor == NULL) {
            host_descriptor = malloc(HOST_DESCRIPTOR_MAX);
        }
        if (host_descriptor == NULL) {
            return BTC_ERR_NOMEM;
        }
    }
    memset(&h, 0, sizeof(h));
    h.host = true;
    have_last = false;
    recall_last();
    h.started = true;
    esp_err_t err = esp_bt_hid_host_register_callback(hidh_cb);
    if (err == ESP_OK) {
        err = esp_bt_hid_host_init();
    }
    for (int i = 0; err == ESP_OK && i < 100 && !h.ready && !h.stopping; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK || !h.ready) {
        ESP_LOGW(TAG, "hid: host didn't start: %s", esp_err_to_name(err));
        btc_hid_stop();
        return err != ESP_OK ? btc_err(err) : BTC_ERR_FAIL;
    }
    return BTC_OK;
}

int btc_hid_host_connect(const uint8_t *addr, bool boot) {
    if (!h.started || !h.host) {
        return BTC_ERR_STATE;
    }
    h.want_boot = boot;
    if (h.connected) {
        if (boot != h.boot) {
            return btc_err(esp_bt_hid_host_set_protocol(h.peer, boot ? ESP_HIDH_BOOT_MODE : ESP_HIDH_REPORT_MODE));
        }
        return BTC_OK;
    }
    esp_bd_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else if (have_last) {
        memcpy(a, last, 6);
    } else {
        return BTC_ERR_ARG;
    }
    return btc_err(esp_bt_hid_host_connect(a));
}

size_t btc_hid_host_read(uint8_t *dst, size_t size) {
    if (!h.started || !h.host) {
        return 0;
    }
    BTC_LOCK();
    size_t n = queue_get(dst, size);
    BTC_UNLOCK();
    return n;
}

int btc_hid_host_send(uint8_t report_id, const uint8_t *data, size_t len) {
    if (!h.connected || !h.host || len > MAX_REPORT) {
        return BTC_ERR_STATE;
    }
    uint8_t buf[MAX_REPORT + 1];
    size_t n = 0;
    if (report_id) {
        buf[n++] = report_id;
    }
    memcpy(buf + n, data, len);
    n += len;
    if (n == 0) {
        return BTC_ERR_ARG;
    }
    esp_bd_addr_t a;
    memcpy(a, h.peer, 6);
    return btc_err(esp_bt_hid_host_send_data(a, buf, n));
}

size_t btc_hid_host_descriptor(uint8_t *dst, size_t size) {
    if (!h.connected || !h.host || host_descriptor == NULL) {
        return 0;
    }
    BTC_LOCK();
    size_t n = h.descriptor_len < size ? h.descriptor_len : size;
    memcpy(dst, host_descriptor, n);
    BTC_UNLOCK();
    return n;
}

static void host_stop(void) {
    if (h.connected) {
        esp_bd_addr_t a;
        memcpy(a, h.peer, 6);
        esp_bt_hid_host_disconnect(a);
        for (int i = 0; i < 50 && h.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    h.ready = true;
    if (esp_bt_hid_host_deinit() == ESP_OK) {
        for (int i = 0; i < 100 && h.ready; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

#else

int btc_hid_host_start(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_host_connect(const uint8_t *addr, bool boot) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_host_read(uint8_t *dst, size_t size) {
    return 0;
}
int btc_hid_host_send(uint8_t report_id, const uint8_t *data, size_t len) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_host_descriptor(uint8_t *dst, size_t size) {
    return 0;
}
static void host_stop(void) {
}

#endif // CONFIG_BT_HID_HOST_ENABLED

// =========================================================================
// both
// =========================================================================

int btc_hid_disconnect(void) {
    if (!h.started) {
        return BTC_ERR_STATE;
    }
    if (!h.connected) {
        return BTC_OK;
    }
    #if CONFIG_BT_HID_HOST_ENABLED
    if (h.host) {
        esp_bd_addr_t a;
        memcpy(a, h.peer, 6);
        return btc_err(esp_bt_hid_host_disconnect(a));
    }
    #endif
    #if CONFIG_BT_HID_DEVICE_ENABLED
    if (!h.host) {
        return btc_err(esp_bt_hid_device_disconnect());
    }
    #endif
    return BTC_ERR_UNSUPPORTED;
}

void btc_hid_stop(void) {
    if (!h.started) {
        return;
    }
    h.stopping = true;
    if (h.host) {
        host_stop();
    } else {
        device_stop();
    }
    BTC_LOCK();
    h.started = false;
    h.connected = false;
    h.ready = false;
    h.q.head = h.q.count = 0;
    BTC_UNLOCK();
}

void btc_hid_status(btc_hid_status_t *out) {
    BTC_LOCK();
    out->started = h.started;
    out->host = h.host;
    out->connected = h.connected;
    memcpy(out->peer, h.peer, 6);
    out->boot = h.boot;
    out->sent = h.sent;
    out->received = h.received;
    out->dropped = h.dropped;
    out->queued = h.q.count + (h.host ? 0 : h.flight_len);
    BTC_UNLOCK();
}

uint32_t btc_hid_changes(void) {
    BTC_LOCK();
    uint32_t c = h.changes;
    h.changes = 0;
    BTC_UNLOCK();
    return c;
}

#else // no HID in this build

int btc_hid_device_start(const uint8_t *desc, size_t len, uint8_t subclass, const char *name, bool boot) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_device_connect(const uint8_t *addr) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_device_send(const uint8_t *report, size_t len) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_device_output(uint8_t *dst, size_t size) {
    return 0;
}
int btc_hid_host_start(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_hid_host_connect(const uint8_t *addr, bool boot) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_host_read(uint8_t *dst, size_t size) {
    return 0;
}
int btc_hid_host_send(uint8_t report_id, const uint8_t *data, size_t len) {
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_hid_host_descriptor(uint8_t *dst, size_t size) {
    return 0;
}
int btc_hid_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}
void btc_hid_stop(void) {
}
void btc_hid_status(btc_hid_status_t *out) {
    memset(out, 0, sizeof(*out));
}
uint32_t btc_hid_changes(void) {
    return 0;
}

#endif
