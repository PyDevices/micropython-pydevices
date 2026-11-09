// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// SDP records of the program's own: a service class UUID and a name, pointing
// at an RFCOMM channel (a Serial port) or an L2CAP PSM, so a device that
// searches finds the service by what it is. Bluedroid builds the record
// (esp_sdp_create_record, type RAW) and shares its SDP task with the profiles
// that keep records (sdp_shared.c).

#include <string.h>

#include "btc_bluedroid.h"

#if CONFIG_BT_SDP_COMMON_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sdp_api.h"

static const char *TAG = "btclassic";

#define SERVICES_MAX 4

static struct {
    bool used;
    int handle;         // Bluedroid's record handle, valid when !pending
    bool pending;       // the create call is out, its event not yet in
    bool failed;
} svc[SERVICES_MAX];
static bool sdp_open;

static void sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param) {
    // SDP is shared: a completion is ours only while one of ours is pending.
    if (event != ESP_SDP_CREATE_RECORD_COMP_EVT) {
        return;
    }
    BTC_LOCK();
    for (int i = 0; i < SERVICES_MAX; i++) {
        if (svc[i].used && svc[i].pending) {
            svc[i].pending = false;
            if (param->create_record.status == ESP_SDP_SUCCESS) {
                svc[i].handle = param->create_record.record_handle;
            } else {
                svc[i].failed = true;
            }
            break;
        }
    }
    BTC_UNLOCK();
}

static void release_sdp_if_idle(void) {
    for (int i = 0; i < SERVICES_MAX; i++) {
        if (svc[i].used) {
            return;
        }
    }
    if (sdp_open) {
        btc_sdp_close(sdp_cb);
        sdp_open = false;
    }
}

int btc_service_add(const btc_uuid_t *uuid, const char *name, int channel, int psm) {
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len >= ESP_SDP_SERVER_NAME_MAX || (channel < 0 && psm < 0)) {
        return BTC_ERR_ARG;
    }
    int slot = -1;
    BTC_LOCK();
    for (int i = 0; i < SERVICES_MAX; i++) {
        if (!svc[i].used) {
            slot = i;
            svc[i].used = true;
            svc[i].pending = true;
            svc[i].failed = false;
            svc[i].handle = 0;
            break;
        }
    }
    BTC_UNLOCK();
    if (slot < 0) {
        return BTC_ERR_NOMEM;
    }
    if (!sdp_open) {
        if (btc_sdp_open(sdp_cb) != ESP_OK) {
            svc[slot].used = false;
            return BTC_ERR_FAIL;
        }
        sdp_open = true;
    }
    esp_bluetooth_sdp_record_t rec = {0};
    rec.raw.hdr.type = ESP_SDP_TYPE_RAW;
    rec.raw.hdr.uuid.len = uuid->len;
    if (uuid->len == 2) {
        rec.raw.hdr.uuid.uuid.uuid16 = (uint16_t)(uuid->uuid[0] << 8 | uuid->uuid[1]);
    } else if (uuid->len == 4) {
        rec.raw.hdr.uuid.uuid.uuid32 = (uint32_t)uuid->uuid[0] << 24 | (uint32_t)uuid->uuid[1] << 16
            | (uint32_t)uuid->uuid[2] << 8 | uuid->uuid[3];
    } else if (uuid->len == 16) {
        // A raw record writes the 128 bits into the record as given, most
        // significant byte first.
        memcpy(rec.raw.hdr.uuid.uuid.uuid128, uuid->uuid, 16);
    } else {
        svc[slot].used = false;
        return BTC_ERR_ARG;
    }
    rec.raw.hdr.service_name = (char *)name;
    rec.raw.hdr.service_name_length = name_len + 1;     // with its NUL, which esp_sdp checks
    rec.raw.hdr.rfcomm_channel_number = channel;        // -1 for none
    rec.raw.hdr.l2cap_psm = psm;
    rec.raw.hdr.profile_version = 0x0100;
    if (esp_sdp_create_record(&rec) != ESP_OK) {
        svc[slot].used = false;
        release_sdp_if_idle();
        return BTC_ERR_FAIL;
    }
    for (int i = 0; i < 100 && svc[slot].pending; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (svc[slot].pending || svc[slot].failed) {
        ESP_LOGW(TAG, "service: the record wasn't made");
        svc[slot].used = false;
        svc[slot].pending = false;
        release_sdp_if_idle();
        return BTC_ERR_FAIL;
    }
    return slot;
}

bool btc_service_active(int slot) {
    return slot >= 0 && slot < SERVICES_MAX && svc[slot].used && !svc[slot].pending;
}

int btc_service_handle(int slot) {
    return btc_service_active(slot) ? svc[slot].handle : 0;
}

void btc_service_remove(int slot) {
    if (!btc_service_active(slot)) {
        return;
    }
    esp_sdp_remove_record(svc[slot].handle);
    svc[slot].used = false;
    release_sdp_if_idle();
}

// The radio is stopping: every record goes with SDP.
void btc_service_stop(void) {
    for (int i = 0; i < SERVICES_MAX; i++) {
        svc[i].used = false;
        svc[i].pending = false;
    }
    if (sdp_open) {
        btc_sdp_close(sdp_cb);
        sdp_open = false;
    }
}

#else

int btc_service_add(const btc_uuid_t *uuid, const char *name, int channel, int psm) {
    (void)uuid;
    (void)name;
    (void)channel;
    (void)psm;
    return BTC_ERR_UNSUPPORTED;
}

bool btc_service_active(int slot) {
    (void)slot;
    return false;
}

int btc_service_handle(int slot) {
    (void)slot;
    return 0;
}

void btc_service_remove(int slot) {
    (void)slot;
}

void btc_service_stop(void) {
}

#endif
