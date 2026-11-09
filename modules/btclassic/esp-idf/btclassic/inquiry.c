// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Discovery on Bluedroid: an inquiry, and the devices it finds with their
// names, classes and signal. radio.c hands every GAP event here. See
// include/btclassic.h.

#include <stdlib.h>
#include <string.h>

#include "esp_gap_bt_api.h"
#include "esp_log.h"

#include "btc_bluedroid.h"

static const char *TAG = "btclassic";

static struct {
    bool running;
    int count;
    btc_found_t found[BTC_FOUND_MAX];
    // Each one's extended inquiry response, on the heap, or NULL.
    uint8_t *eir[BTC_FOUND_MAX];
    uint8_t eir_len[BTC_FOUND_MAX];
} q;

static void mark(void) {
    // The radio's bits live in radio.c; a found device is news to it.
    btc_radio_mark(BTC_CHANGED_FOUND);
}

// The name from an inquiry result: the BDNAME property, or the complete or
// short local name in its extended inquiry response.
static size_t result_name(const esp_bt_gap_cb_param_t *param, char *dst, size_t size) {
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        const uint8_t *src = NULL;
        uint8_t len = 0;
        if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            src = p->val;
            len = (uint8_t)strnlen((const char *)p->val, p->len);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
            src = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (src == NULL) {
                src = esp_bt_gap_resolve_eir_data(p->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            }
        }
        if (src && len) {
            if (len >= size) {
                len = size - 1;
            }
            memcpy(dst, src, len);
            dst[len] = 0;
            return len;
        }
    }
    return 0;
}

static void on_result(const esp_bt_gap_cb_param_t *param) {
    btc_found_t f = {.rssi = BTC_UNKNOWN};
    memcpy(f.addr, param->disc_res.bda, sizeof(btc_addr_t));
    const uint8_t *eir = NULL;
    int eir_len = 0;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        const esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_COD) {
            f.cod = *(const uint32_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI) {
            f.rssi = *(const int8_t *)p->val;
        } else if (p->type == ESP_BT_GAP_DEV_PROP_EIR) {
            eir = p->val;
            eir_len = p->len > BTC_EIR_MAX ? BTC_EIR_MAX : p->len;
        }
    }
    // The EIR as it came, without the zero padding after its last field.
    uint8_t *copy = NULL;
    if (eir) {
        int used = 0;
        while (used < eir_len && eir[used] && used + 1 + eir[used] <= eir_len) {
            used += 1 + eir[used];
        }
        eir_len = used;
        if (eir_len) {
            copy = malloc(eir_len);
            if (copy) {
                memcpy(copy, eir, eir_len);
            }
        }
    }
    result_name(param, f.name, sizeof(f.name));

    BTC_LOCK();
    int at = 0;
    while (at < q.count && memcmp(q.found[at].addr, f.addr, sizeof(btc_addr_t)) != 0) {
        at++;
    }
    bool fresh = at == q.count;
    uint8_t *old_eir = NULL;
    if (fresh && q.count < BTC_FOUND_MAX) {
        q.found[q.count] = f;
        q.eir[q.count] = copy;
        q.eir_len[q.count] = copy ? eir_len : 0;
        q.count++;
        copy = NULL;
    } else if (!fresh) {
        if (copy) {
            old_eir = q.eir[at];
            q.eir[at] = copy;
            q.eir_len[at] = eir_len;
            copy = NULL;
        }
        // A second response: keep what the first had that this one lacks.
        btc_found_t *old = &q.found[at];
        if (f.name[0]) {
            memcpy(old->name, f.name, sizeof(f.name));
        }
        if (f.cod) {
            old->cod = f.cod;
        }
        if (f.rssi != BTC_UNKNOWN) {
            old->rssi = f.rssi;
        }
    }
    BTC_UNLOCK();
    free(copy);
    free(old_eir);
    mark();
}

// Forget the last inquiry's devices and their responses.
static void forget(void) {
    uint8_t *gone[BTC_FOUND_MAX];
    BTC_LOCK();
    int n = q.count;
    for (int i = 0; i < n; i++) {
        gone[i] = q.eir[i];
        q.eir[i] = NULL;
        q.eir_len[i] = 0;
    }
    q.count = 0;
    BTC_UNLOCK();
    for (int i = 0; i < n; i++) {
        free(gone[i]);
    }
}

void btc_inquiry_on_gap(int event, void *arg) {
    esp_bt_gap_cb_param_t *param = arg;
    switch (event) {
        case ESP_BT_GAP_DISC_RES_EVT:
            if (q.running) {
                on_result(param);
            }
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && q.running) {
                q.running = false;
                ESP_LOGI(TAG, "inquiry done: %d found", q.count);
                mark();
            }
            break;
        case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
            if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
                btc_source_on_remote_name(param->read_rmt_name.bda, (const char *)param->read_rmt_name.rmt_name);
            }
            break;
        default:
            break;
    }
}

int btc_radio_inquiry(uint32_t duration_ms) {
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (duration_ms == 0) {
        if (!q.running) {
            return BTC_OK;
        }
        return btc_err(esp_bt_gap_cancel_discovery());
    }
    // In units of 1.28 s, 1 to 48.
    uint32_t units = (duration_ms + 640) / 1280;
    if (units < 1 || units > 0x30) {
        return BTC_ERR_ARG;
    }
    if (q.running) {
        esp_bt_gap_cancel_discovery();
    }
    forget();
    q.running = true;
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (uint8_t)units, 0);
    if (err != ESP_OK) {
        q.running = false;
    }
    return btc_err(err);
}

bool btc_radio_inquiring(void) {
    return q.running;
}

void btc_inquiry_stop(void) {
    q.running = false;
}

int btc_radio_found(btc_found_t *out, int max) {
    BTC_LOCK();
    int n = q.count;
    for (int i = 0; i < n && i < max; i++) {
        out[i] = q.found[i];
    }
    BTC_UNLOCK();
    return n;
}

int btc_radio_found_eir(const uint8_t *addr, uint8_t *dst, size_t size) {
    int len = BTC_ERR_ARG;
    BTC_LOCK();
    for (int i = 0; i < q.count; i++) {
        if (memcmp(q.found[i].addr, addr, sizeof(btc_addr_t)) == 0) {
            len = q.eir_len[i];
            if (len) {
                memcpy(dst, q.eir[i], (size_t)len < size ? (size_t)len : size);
            }
            break;
        }
    }
    BTC_UNLOCK();
    return len;
}
