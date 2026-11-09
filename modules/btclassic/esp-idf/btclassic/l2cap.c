// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// A raw L2CAP channel on Bluedroid: one link at a time, as a server on a PSM
// others connect to and as a client of another device's. See
// include/btclassic.h.
//
// Bluedroid hands an open channel over as a file descriptor (its L2CAP VFS).
// read() on it never waits, and write() queues one packet per MTU, waiting
// only when Bluedroid's own send queue is full. Nothing tells us that bytes
// arrived, so every call from the program first pulls what's waiting into
// the receive ring (pull()).

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_l2cap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "btc_bluedroid.h"

static const char *TAG = "btclassic";

static struct {
    bool started;
    bool initialised;       // ESP_BT_L2CAP_INIT_EVT came
    bool vfs;               // the VFS is registered
    bool paused;            // Bluedroid's side is down while the serial port starts
    bool parked;            // stopped, but left up underneath for the serial port
    bool srv_stopped;       // ESP_BT_L2CAP_SRV_STOP_EVT came
    uint16_t psm;           // the server's PSM, 0 for none
    int fd;                 // the open channel, -1 with none
    bool calling;
    uint8_t *rx;
    uint32_t rx_size, rx_head, rx_count;
    uint32_t changes;
    btc_l2cap_status_t st;
} l;

// Authenticated and encrypted, as a serial link is.
#define SEC_MASK (ESP_BT_L2CAP_SEC_AUTHENTICATE | ESP_BT_L2CAP_SEC_ENCRYPT)

static void serve(void) {
    if (l.psm && l.vfs && !l.paused) {
        esp_bt_l2cap_start_srv(SEC_MASK, l.psm);
    }
}

static void l2cap_cb(esp_bt_l2cap_cb_event_t event, esp_bt_l2cap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_L2CAP_INIT_EVT:
            if (param->init.status == ESP_BT_L2CAP_SUCCESS) {
                l.initialised = true;
                esp_bt_l2cap_vfs_register();
            } else {
                ESP_LOGW(TAG, "l2cap: init failed: %d", param->init.status);
            }
            break;
        case ESP_BT_L2CAP_UNINIT_EVT:
            l.initialised = false;
            break;
        case ESP_BT_L2CAP_VFS_REGISTER_EVT:
            l.vfs = param->vfs_register.status == ESP_BT_L2CAP_SUCCESS;
            serve();
            break;
        case ESP_BT_L2CAP_VFS_UNREGISTER_EVT:
            l.vfs = false;
            break;
        case ESP_BT_L2CAP_START_EVT:
            if (param->start.status == ESP_BT_L2CAP_SUCCESS) {
                l.st.listening = true;
            } else {
                ESP_LOGW(TAG, "l2cap: serving PSM 0x%04x failed: %d", l.psm, param->start.status);
            }
            break;
        case ESP_BT_L2CAP_SRV_STOP_EVT:
            l.st.listening = false;
            l.srv_stopped = true;
            break;
        case ESP_BT_L2CAP_CL_INIT_EVT:
            if (param->cl_init.status != ESP_BT_L2CAP_SUCCESS) {
                l.calling = false;
            }
            break;
        case ESP_BT_L2CAP_OPEN_EVT:
            BTC_LOCK();
            l.calling = false;
            if (param->open.status == ESP_BT_L2CAP_SUCCESS) {
                l.fd = param->open.fd;
                l.st.connected = true;
                l.st.mtu = param->open.tx_mtu > 0xffff ? 0xffff : (uint16_t)param->open.tx_mtu;
                memcpy(l.st.peer, param->open.rem_bda, sizeof(btc_addr_t));
                // A server takes one channel; it's served again once that closes.
                l.st.listening = false;
            }
            l.changes |= BTC_CHANGED_CONNECTION;
            BTC_UNLOCK();
            break;
        case ESP_BT_L2CAP_CLOSE_EVT:
            BTC_LOCK();
            l.fd = -1;
            l.calling = false;
            l.st.connected = false;
            l.changes |= BTC_CHANGED_CONNECTION;
            BTC_UNLOCK();
            if (l.started) {
                serve();
            }
            break;
        default:
            break;
    }
}

// Move what Bluedroid holds for us into the receive ring, as far as it has
// room: the rest waits in Bluedroid's queue. MicroPython's task.
static void pull(void) {
    uint8_t buf[256];
    for (;;) {
        int fd = l.fd;
        if (fd < 0 || !l.rx) {
            return;
        }
        size_t room = l.rx_size - l.rx_count;
        if (room == 0) {
            return;
        }
        ssize_t n = read(fd, buf, room < sizeof(buf) ? room : sizeof(buf));
        if (n <= 0) {
            return;
        }
        BTC_LOCK();
        for (ssize_t i = 0; i < n; i++) {
            if (l.rx_count < l.rx_size) {
                l.rx[(l.rx_head + l.rx_count) % l.rx_size] = buf[i];
                l.rx_count++;
            } else {
                l.st.rx_dropped++;
            }
        }
        l.st.rx_bytes += n;
        l.changes |= BTC_CHANGED_L2CAP;
        BTC_UNLOCK();
    }
}

// Bring Bluedroid's side up: its INIT_EVT registers the VFS, and that serves
// the PSM.
static esp_err_t sdk_up(void) {
    esp_err_t err = esp_bt_l2cap_register_callback(l2cap_cb);
    if (err == ESP_OK) {
        err = esp_bt_l2cap_init();
    }
    return err;
}

// Take Bluedroid's side down: the channel, the VFS and L2CAP itself.
static void sdk_down(void) {
    if (l.fd >= 0) {
        close(l.fd);
    }
    // The VFS goes before L2CAP: Bluedroid refuses to unregister it after.
    if (l.vfs) {
        esp_bt_l2cap_vfs_unregister();
        for (int i = 0; i < 50 && l.vfs; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (l.initialised) {
        esp_bt_l2cap_deinit();     // closes the channel and the server
        for (int i = 0; i < 50 && l.initialised; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    BTC_LOCK();
    if (l.fd >= 0 || l.st.connected) {
        l.changes |= BTC_CHANGED_CONNECTION;
    }
    l.fd = -1;
    l.calling = false;
    l.st.connected = false;
    l.st.listening = false;
    BTC_UNLOCK();
}

// Close the channel and stop serving, but leave L2CAP and its VFS up: what
// a stop does while the serial port runs, since taking L2CAP down would
// take the serial port's layer down too (btc_bluedroid.h).
static void park(void) {
    if (l.fd >= 0) {
        close(l.fd);
    }
    if (l.psm) {
        l.srv_stopped = false;
        if (esp_bt_l2cap_stop_srv(l.psm) == ESP_OK) {
            for (int i = 0; i < 50 && !l.srv_stopped; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    BTC_LOCK();
    l.fd = -1;
    l.calling = false;
    l.st.connected = false;
    l.st.listening = false;
    BTC_UNLOCK();
    l.parked = true;
}

int btc_l2cap_start(uint16_t psm, size_t rx_bytes) {
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (l.started) {
        return BTC_OK;
    }
    uint8_t *rx = heap_caps_malloc(rx_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rx == NULL) {
        rx = malloc(rx_bytes);
    }
    if (rx == NULL) {
        return BTC_ERR_NOMEM;
    }
    // A parked L2CAP is still up underneath: serve the new PSM on it.
    bool parked = l.parked, initialised = l.initialised, vfs = l.vfs;
    memset(&l, 0, sizeof(l));
    l.fd = -1;
    l.psm = psm;
    l.rx = rx;
    l.rx_size = (uint32_t)rx_bytes;
    l.st.psm = psm;
    if (parked && initialised) {
        l.initialised = true;
        l.vfs = vfs;
        l.started = true;
        serve();
        return BTC_OK;
    }
    esp_err_t err = sdk_up();
    if (err != ESP_OK) {
        free(rx);
        memset(&l, 0, sizeof(l));
        l.fd = -1;
        return btc_err(err);
    }
    l.started = true;
    return BTC_OK;
}

void btc_l2cap_stop(void) {
    if (!l.started) {
        return;
    }
    l.started = false;     // so a close doesn't serve again
    if (btc_spp_started()) {
        park();
    } else {
        sdk_down();
        btc_spp_unpark();
    }
    BTC_LOCK();
    uint8_t *rx = l.rx;
    l.rx = NULL;
    l.rx_count = 0;
    BTC_UNLOCK();
    free(rx);
    l.paused = false;
}

// See btc_bluedroid.h: the serial port is starting, and has to bring up the
// layer the two share itself.
void btc_l2cap_pause(void) {
    if (l.parked) {
        sdk_down();
        l.parked = false;
    }
    if (!l.started || l.paused) {
        return;
    }
    l.paused = true;
    sdk_down();
}

void btc_l2cap_resume(void) {
    if (!l.started || !l.paused) {
        l.paused = false;
        return;
    }
    l.paused = false;
    esp_err_t err = sdk_up();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "l2cap: restart failed: %d", err);
    }
}

// The serial port stopped for good: a parked L2CAP comes down with it.
void btc_l2cap_unpark(void) {
    if (l.parked && !l.started) {
        sdk_down();
    }
    l.parked = false;
}

bool btc_l2cap_up(void) {
    return l.started || l.parked;
}

bool btc_l2cap_started(void) {
    return l.started;
}

bool btc_l2cap_connected(void) {
    return l.fd >= 0;
}

int btc_l2cap_connect(const uint8_t *addr, uint16_t psm) {
    // Just after starting, Bluedroid is still bringing the channel's VFS up.
    for (int i = 0; i < 100 && l.started && !l.vfs; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!l.started || !l.vfs) {
        return BTC_ERR_STATE;
    }
    if (l.fd >= 0 || l.calling) {
        return BTC_ERR_STATE;
    }
    esp_bd_addr_t bda;
    memcpy(bda, addr, sizeof(bda));
    l.calling = true;
    esp_err_t err = esp_bt_l2cap_connect(SEC_MASK, psm, bda);
    if (err != ESP_OK) {
        l.calling = false;
        return btc_err(err);
    }
    return BTC_OK;
}

bool btc_l2cap_calling(void) {
    return l.calling;
}

int btc_l2cap_disconnect(void) {
    int fd = l.fd;
    if (fd < 0) {
        return BTC_ERR_STATE;
    }
    close(fd);
    return BTC_OK;
}

size_t btc_l2cap_any(void) {
    pull();
    return l.rx_count;
}

size_t btc_l2cap_read(uint8_t *dst, size_t size) {
    pull();
    BTC_LOCK();
    size_t n = size < l.rx_count ? size : l.rx_count;
    for (size_t i = 0; i < n; i++) {
        dst[i] = l.rx[(l.rx_head + i) % l.rx_size];
    }
    l.rx_head = (l.rx_head + n) % (l.rx_size ? l.rx_size : 1);
    l.rx_count -= n;
    BTC_UNLOCK();
    return n;
}

size_t btc_l2cap_write(const uint8_t *src, size_t size) {
    int fd = l.fd;
    if (fd < 0 || size == 0) {
        return 0;
    }
    ssize_t n = write(fd, src, size);
    if (n <= 0) {
        return 0;
    }
    l.st.tx_bytes += n;
    return (size_t)n;
}

size_t btc_l2cap_write_room(void) {
    return l.fd >= 0 ? (l.st.mtu ? l.st.mtu : 1) : 0;
}

uint32_t btc_l2cap_changes(void) {
    pull();
    BTC_LOCK();
    uint32_t c = l.changes;
    l.changes = 0;
    BTC_UNLOCK();
    return c;
}

void btc_l2cap_status(btc_l2cap_status_t *out) {
    pull();
    BTC_LOCK();
    *out = l.st;
    out->connected = l.fd >= 0;
    BTC_UNLOCK();
}
