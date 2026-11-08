// Bluedroid's SDP API (esp_sdp) takes one callback and starts and stops as a
// whole. HIDDevice's Device ID record and Messages both use it, so they share
// it here: each opens it with its own callback, every event goes to each,
// and it stops when the last one closes.

#include "btc_bluedroid.h"

#if CONFIG_BT_SDP_COMMON_ENABLED

#include "freertos/task.h"
#include "esp_sdp_api.h"

#define SDP_USERS 4

static esp_sdp_cb_t users[SDP_USERS];
static volatile int8_t state;   // 0 off, 1 starting, 2 on, -1 failed

static void dispatch(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param) {
    if (event == ESP_SDP_INIT_EVT) {
        state = param->init.status == ESP_SDP_SUCCESS ? 2 : -1;
    } else if (event == ESP_SDP_DEINIT_EVT) {
        state = 0;
    }
    for (int i = 0; i < SDP_USERS; i++) {
        esp_sdp_cb_t cb = users[i];
        if (cb) {
            cb(event, param);
        }
    }
}

static int count(void) {
    int n = 0;
    for (int i = 0; i < SDP_USERS; i++) {
        n += users[i] != NULL;
    }
    return n;
}

esp_err_t btc_sdp_open(esp_sdp_cb_t cb) {
    int slot = -1;
    for (int i = 0; i < SDP_USERS; i++) {
        if (users[i] == cb) {
            slot = i;
            break;
        }
        if (users[i] == NULL && slot < 0) {
            slot = i;
        }
    }
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    users[slot] = cb;
    if (state != 2) {
        state = 1;
        esp_sdp_register_callback(dispatch);
        esp_err_t err = esp_sdp_init();
        if (err == ESP_ERR_INVALID_STATE) {
            state = 2;      // already running
        } else if (err != ESP_OK) {
            state = 0;
            users[slot] = NULL;
            return err;
        }
        for (int i = 0; i < 100 && state == 1; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (state != 2) {
            state = 0;
            users[slot] = NULL;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

void btc_sdp_close(esp_sdp_cb_t cb) {
    for (int i = 0; i < SDP_USERS; i++) {
        if (users[i] == cb) {
            users[i] = NULL;
        }
    }
    if (count() == 0 && state == 2) {
        if (esp_sdp_deinit() == ESP_OK) {
            for (int i = 0; i < 50 && state != 0; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        state = 0;
    }
}

#endif
