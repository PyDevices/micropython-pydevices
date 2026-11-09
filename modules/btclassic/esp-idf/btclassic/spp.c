// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The serial port on Bluedroid: SPP over RFCOMM, as a server others connect
// to (a PC's Bluetooth COM port, a phone's terminal app) and as a client of
// another device. One link at a time; bytes in and out through two rings.
// See include/btclassic.h.
//
// Bluedroid's callback mode hands received bytes to spp_cb on its task, and
// takes one write at a time: the next goes once ESP_SPP_WRITE_EVT says the
// last is done and the link isn't congested. esp_spp_write copies the bytes.

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "btc_bluedroid.h"

static const char *TAG = "btclassic";

#define TX_SIZE 4096
#define CHUNK 990       // RFCOMM's default frame size on Bluedroid

typedef struct {
    uint8_t *buf;
    uint32_t size, head, count;
} bytes_ring_t;

static struct {
    bool started;
    bool initialised;       // ESP_SPP_INIT_EVT came
    bool server;            // the server is listening
    char name[32];
    uint32_t handle;        // the link, 0 with none
    btc_addr_t connecting;  // a client call waiting for SDP
    bool calling;
    uint32_t call_handle;   // the client call's handle, once the stack gives one
    bytes_ring_t rx, tx;
    uint32_t in_flight;     // bytes handed to esp_spp_write, not yet done
    bool congested;
    uint32_t changes;
    btc_spp_status_t st;
} p;

static void mark(uint32_t bits) {
    BTC_LOCK();
    p.changes |= bits;
    BTC_UNLOCK();
}

static void ring_put(bytes_ring_t *r, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        r->buf[(r->head + r->count + i) % r->size] = src[i];
    }
    r->count += n;
}

// Send the next chunk if nothing is in flight. Any task.
static void tx_kick(void) {
    uint8_t chunk[CHUNK];
    BTC_LOCK();
    uint32_t handle = p.handle;
    uint32_t n = 0;
    if (handle && !p.in_flight && !p.congested && p.tx.count) {
        n = p.tx.count < CHUNK ? p.tx.count : CHUNK;
        for (uint32_t i = 0; i < n; i++) {
            chunk[i] = p.tx.buf[(p.tx.head + i) % p.tx.size];
        }
        p.in_flight = n;
    }
    BTC_UNLOCK();
    if (n && esp_spp_write(handle, (int)n, chunk) != ESP_OK) {
        BTC_LOCK();
        p.in_flight = 0;
        BTC_UNLOCK();
    }
}

static void opened(uint32_t handle, const uint8_t *addr) {
    BTC_LOCK();
    p.handle = handle;
    p.in_flight = 0;
    p.congested = false;
    p.st.connected = true;
    memcpy(p.st.peer, addr, sizeof(btc_addr_t));
    p.changes |= BTC_CHANGED_CONNECTION;
    BTC_UNLOCK();
    tx_kick();
}

static void spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
    switch (event) {
        case ESP_SPP_INIT_EVT:
            if (param->init.status == ESP_SPP_SUCCESS) {
                p.initialised = true;
                esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, p.name);
            } else {
                ESP_LOGW(TAG, "spp: init failed: %d", param->init.status);
            }
            break;
        case ESP_SPP_UNINIT_EVT:
            p.initialised = false;
            p.server = false;
            break;
        case ESP_SPP_START_EVT:
            if (param->start.status == ESP_SPP_SUCCESS) {
                p.server = true;
                p.st.channel = param->start.scn;
                ESP_LOGI(TAG, "spp: serving on channel %u", param->start.scn);
            }
            break;
        case ESP_SPP_SRV_OPEN_EVT:
            if (param->srv_open.status != ESP_SPP_SUCCESS) {
                break;
            }
            if (p.handle) {
                esp_spp_disconnect(param->srv_open.handle);    // one link at a time
                break;
            }
            opened(param->srv_open.handle, param->srv_open.rem_bda);
            break;
        case ESP_SPP_DISCOVERY_COMP_EVT:
            if (!p.calling) {
                break;
            }
            if (param->disc_comp.status == ESP_SPP_SUCCESS && param->disc_comp.scn_num > 0) {
                ESP_LOGI(TAG, "spp: calling channel %u", param->disc_comp.scn[0]);
                esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER, param->disc_comp.scn[0], p.connecting);
            } else {
                ESP_LOGW(TAG, "spp: no serial port found");
                p.calling = false;
                mark(BTC_CHANGED_CONNECTION);
            }
            break;
        case ESP_SPP_CL_INIT_EVT:
            // A call that fails (the device is away) ends with a close on
            // this handle, not an open.
            if (p.calling) {
                if (param->cl_init.status == ESP_SPP_SUCCESS) {
                    p.call_handle = param->cl_init.handle;
                } else {
                    ESP_LOGW(TAG, "spp: call failed: %d", param->cl_init.status);
                    p.calling = false;
                    mark(BTC_CHANGED_CONNECTION);
                }
            }
            break;
        case ESP_SPP_OPEN_EVT:
            p.calling = false;
            p.call_handle = 0;
            if (param->open.status != ESP_SPP_SUCCESS) {
                ESP_LOGW(TAG, "spp: call failed: %d", param->open.status);
                mark(BTC_CHANGED_CONNECTION);
                break;
            }
            if (p.handle) {
                esp_spp_disconnect(param->open.handle);
                break;
            }
            opened(param->open.handle, param->open.rem_bda);
            break;
        case ESP_SPP_CLOSE_EVT:
            BTC_LOCK();
            if (p.calling && p.call_handle && param->close.handle == p.call_handle) {
                p.calling = false;
                p.call_handle = 0;
                p.changes |= BTC_CHANGED_CONNECTION;
            }
            if (param->close.handle == p.handle) {
                p.handle = 0;
                p.st.connected = false;
                p.in_flight = 0;
                p.tx.head = p.tx.count = 0;
                p.changes |= BTC_CHANGED_CONNECTION;
            }
            BTC_UNLOCK();
            break;
        case ESP_SPP_DATA_IND_EVT: {
            uint32_t n = param->data_ind.len;
            BTC_LOCK();
            uint32_t room = p.rx.size - p.rx.count;
            uint32_t take = n < room ? n : room;
            ring_put(&p.rx, param->data_ind.data, take);
            p.st.rx_bytes += n;
            p.st.rx_dropped += n - take;
            p.changes |= BTC_CHANGED_SERIAL;
            BTC_UNLOCK();
            break;
        }
        case ESP_SPP_WRITE_EVT:
            BTC_LOCK();
            if (p.in_flight) {
                // Done or failed, the bytes are spent: a failed write means
                // the link is going, and its close empties the ring anyway.
                uint32_t n = p.in_flight;
                p.tx.head = (p.tx.head + n) % p.tx.size;
                p.tx.count -= n;
                if (param->write.status == ESP_SPP_SUCCESS) {
                    p.st.tx_bytes += n;
                }
                p.in_flight = 0;
            }
            p.congested = param->write.cong;
            BTC_UNLOCK();
            tx_kick();
            break;
        case ESP_SPP_CONG_EVT:
            p.congested = param->cong.cong;
            if (!p.congested) {
                tx_kick();
            }
            break;
        default:
            break;
    }
}

int btc_spp_start(const char *name, size_t rx_bytes) {
    if (p.started) {
        return BTC_ERR_STATE;
    }
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (rx_bytes < 256) {
        rx_bytes = 256;
    }
    uint8_t *rx = heap_caps_malloc(rx_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rx == NULL) {
        rx = malloc(rx_bytes);
    }
    uint8_t *tx = malloc(TX_SIZE);
    if (rx == NULL || tx == NULL) {
        free(rx);
        free(tx);
        return BTC_ERR_NOMEM;
    }
    memset(&p, 0, sizeof(p));
    strncpy(p.name, name && *name ? name : "Serial Port", sizeof(p.name) - 1);
    p.rx = (bytes_ring_t){.buf = rx, .size = (uint32_t)rx_bytes};
    p.tx = (bytes_ring_t){.buf = tx, .size = TX_SIZE};
    esp_err_t err = esp_spp_register_callback(spp_cb);
    if (err == ESP_OK) {
        esp_spp_cfg_t cfg = {.mode = ESP_SPP_MODE_CB, .enable_l2cap_ertm = true, .tx_buffer_size = 0};
        err = esp_spp_enhanced_init(&cfg);
    }
    if (err != ESP_OK) {
        free(rx);
        free(tx);
        memset(&p, 0, sizeof(p));
        return btc_err(err);
    }
    p.started = true;
    btc_radio_profile(BTC_PROFILE_SERIAL, true, 0);
    return BTC_OK;
}

void btc_spp_stop(void) {
    if (!p.started) {
        return;
    }
    if (p.initialised) {
        esp_spp_deinit();   // closes the link and the server
        for (int i = 0; i < 50 && p.initialised; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    btc_radio_profile(BTC_PROFILE_SERIAL, false, 0);
    BTC_LOCK();
    p.started = false;
    p.handle = 0;
    uint8_t *rx = p.rx.buf, *tx = p.tx.buf;
    p.rx.buf = p.tx.buf = NULL;
    p.rx.count = p.tx.count = 0;
    p.st.connected = false;
    BTC_UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(20));  // let a callback that read the rings finish
    free(rx);
    free(tx);
}

bool btc_spp_started(void) {
    return p.started;
}

bool btc_spp_connected(void) {
    return p.handle != 0;
}

int btc_spp_connect(const uint8_t *addr, int channel) {
    if (!p.started || !p.initialised || addr == NULL) {
        return BTC_ERR_STATE;
    }
    if (p.handle || p.calling) {
        return BTC_ERR_STATE;
    }
    memcpy(p.connecting, addr, sizeof(btc_addr_t));
    p.calling = true;
    p.call_handle = 0;
    esp_err_t err = channel > 0
        ? esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_MASTER, (uint8_t)channel, p.connecting)
        : esp_spp_start_discovery(p.connecting);
    if (err != ESP_OK) {
        p.calling = false;
    }
    return btc_err(err);
}

int btc_spp_disconnect(void) {
    if (!p.handle) {
        return BTC_ERR_STATE;
    }
    return btc_err(esp_spp_disconnect(p.handle));
}

size_t btc_spp_any(void) {
    return p.started ? p.rx.count : 0;
}

size_t btc_spp_read(uint8_t *dst, size_t size) {
    BTC_LOCK();
    size_t n = 0;
    if (p.rx.buf) {
        n = size < p.rx.count ? size : p.rx.count;
        for (size_t i = 0; i < n; i++) {
            dst[i] = p.rx.buf[(p.rx.head + i) % p.rx.size];
        }
        p.rx.head = (p.rx.head + n) % p.rx.size;
        p.rx.count -= n;
    }
    BTC_UNLOCK();
    return n;
}

size_t btc_spp_write(const uint8_t *src, size_t size) {
    BTC_LOCK();
    size_t n = 0;
    if (p.handle && p.tx.buf) {
        uint32_t room = p.tx.size - p.tx.count;
        n = size < room ? size : room;
        ring_put(&p.tx, src, (uint32_t)n);
    }
    BTC_UNLOCK();
    if (n) {
        tx_kick();
    }
    return n;
}

size_t btc_spp_write_room(void) {
    return p.handle && p.tx.buf ? p.tx.size - p.tx.count : 0;
}

uint32_t btc_spp_changes(void) {
    BTC_LOCK();
    uint32_t c = p.changes;
    p.changes = 0;
    BTC_UNLOCK();
    return c;
}

void btc_spp_status(btc_spp_status_t *out) {
    BTC_LOCK();
    *out = p.st;
    out->connected = p.handle != 0;
    out->listening = p.server;
    BTC_UNLOCK();
}
