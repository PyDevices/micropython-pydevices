// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The phone book on Bluedroid's PBAP client (esp_pbac): a phone's contacts
// and call history, as vCards. See include/btclassic.h.
//
// One request runs at a time. A listing or a single entry first sets the
// phone's current folder, then asks; the reply comes in pieces, gathered
// into one buffer (PSRAM when there is some) that grows to `max_bytes`.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "btc_bluedroid.h"

#if CONFIG_BT_PBAC_ENABLED

#include "esp_pbac_api.h"

static const char *TAG = "btclassic";

#define PB_NAME_MAX 64         // a handle, or a search

typedef enum {
    OP_NONE = 0,
    OP_PULL,
    OP_LIST,
    OP_ENTRY,
} op_t;

static struct {
    bool started;
    bool connected;
    esp_pbac_conn_hdl_t handle;
    btc_addr_t peer;
    uint8_t repositories;
    uint32_t features;
    uint32_t changes;

    // The request.
    op_t op;
    bool busy;
    bool setting_path;      // waiting for the folder to be set first
    int result;
    bool truncated;
    int book_size, new_missed;
    char name[PB_NAME_MAX];    // the handle to fetch, after the folder is set
    char search[PB_NAME_MAX];
    esp_pbac_pull_vcard_listing_app_param_t list_param;
    esp_pbac_pull_vcard_entry_app_param_t entry_param;

    // The reply. Written by Bluedroid's task while busy, read by callers
    // after; `len` is published under the lock.
    uint8_t *buf;
    size_t len, cap, max;
} p;

static void mark(uint32_t bits) {
    BTC_LOCK();
    p.changes |= bits;
    BTC_UNLOCK();
}

static void finish(int result) {
    BTC_LOCK();
    p.result = result;
    p.busy = false;
    p.setting_path = false;
    p.op = OP_NONE;
    BTC_UNLOCK();
    mark(BTC_CHANGED_PHONEBOOK);
}

// Add a piece of the reply, growing the buffer by doubling up to the cap.
static void gather(const uint8_t *data, size_t n) {
    if (n == 0 || data == NULL) {
        return;
    }
    size_t need = p.len + n;
    if (need > p.max) {
        p.truncated = true;
        need = p.max;
        n = need - p.len;
        if (n == 0) {
            return;
        }
    }
    if (need > p.cap) {
        size_t cap = p.cap ? p.cap : 4096;
        while (cap < need) {
            cap *= 2;
        }
        if (cap > p.max) {
            cap = p.max;
        }
        uint8_t *nb = heap_caps_realloc(p.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            nb = heap_caps_realloc(p.buf, cap, MALLOC_CAP_8BIT);
        }
        if (nb == NULL) {
            p.truncated = true;
            return;
        }
        BTC_LOCK();
        p.buf = nb;
        p.cap = cap;
        BTC_UNLOCK();
    }
    memcpy(p.buf + p.len, data, n);
    BTC_LOCK();
    p.len += n;
    BTC_UNLOCK();
}

// The application parameters that ride on a reply.
static void sizes(bool has_size, uint16_t size, bool has_missed, uint8_t missed) {
    BTC_LOCK();
    if (has_size) {
        p.book_size = size;
    }
    if (has_missed) {
        p.new_missed = missed;
    }
    BTC_UNLOCK();
}

static int reply_result(esp_pbac_status_t st) {
    return st == ESP_PBAC_SUCCESS ? BTC_OK : (int)st;
}

static void pb_cb(esp_pbac_event_t event, esp_pbac_param_t *param) {
    switch (event) {
        case ESP_PBAC_CONNECTION_STATE_EVT: {
            bool up = param->conn_stat.connected;
            BTC_LOCK();
            bool was = p.connected;
            p.connected = up;
            if (up) {
                p.handle = param->conn_stat.handle;
                p.repositories = param->conn_stat.peer_supported_repo;
                p.features = param->conn_stat.peer_supported_feat;
                memcpy(p.peer, param->conn_stat.remote_bda, sizeof(btc_addr_t));
            } else {
                p.handle = ESP_PBAC_INVALID_HANDLE;
            }
            BTC_UNLOCK();
            if (up) {
                ESP_LOGI(TAG, "phone book connected: repositories 0x%x, features 0x%x",
                    p.repositories, (unsigned)p.features);
            } else {
                ESP_LOGI(TAG, "phone book disconnected: %d", param->conn_stat.reason);
            }
            if (!up && p.busy) {
                finish(BTC_ERR_STATE);
            } else if (!up && !was) {
                // A connect that failed: say why in result.
                BTC_LOCK();
                p.result = param->conn_stat.reason == ESP_PBAC_SUCCESS ? BTC_ERR_FAIL : (int)param->conn_stat.reason;
                BTC_UNLOCK();
            }
            mark(BTC_CHANGED_CONNECTION | BTC_CHANGED_PHONEBOOK);
            break;
        }
        case ESP_PBAC_PULL_PHONE_BOOK_RESPONSE_EVT: {
            struct pbac_pull_phone_book_rsp_param *r = &param->pull_phone_book_rsp;
            if (p.op != OP_PULL) {
                break;
            }
            if (r->result != ESP_PBAC_SUCCESS) {
                finish(reply_result(r->result));
                break;
            }
            sizes(r->include_phone_book_size, r->phone_book_size, r->include_new_missed_calls, r->new_missed_calls);
            gather(r->data, r->data_len);
            if (r->final) {
                finish(BTC_OK);
            }
            break;
        }
        case ESP_PBAC_SET_PHONE_BOOK_RESPONSE_EVT: {
            if (!p.setting_path) {
                break;
            }
            p.setting_path = false;
            if (param->set_phone_book_rsp.result != ESP_PBAC_SUCCESS) {
                finish(reply_result(param->set_phone_book_rsp.result));
                break;
            }
            esp_err_t err = ESP_FAIL;
            if (p.op == OP_LIST) {
                err = esp_pbac_pull_vcard_listing(p.handle, "", &p.list_param);
            } else if (p.op == OP_ENTRY) {
                err = esp_pbac_pull_vcard_entry(p.handle, p.name, &p.entry_param);
            }
            if (err != ESP_OK) {
                finish(btc_err(err));
            }
            break;
        }
        case ESP_PBAC_PULL_VCARD_LISTING_RESPONSE_EVT: {
            struct pbac_pull_vcard_listing_rsp_param *r = &param->pull_vcard_listing_rsp;
            if (p.op != OP_LIST) {
                break;
            }
            if (r->result != ESP_PBAC_SUCCESS) {
                finish(reply_result(r->result));
                break;
            }
            sizes(r->include_phone_book_size, r->phone_book_size, r->include_new_missed_calls, r->new_missed_calls);
            gather(r->data, r->data_len);
            if (r->final) {
                finish(BTC_OK);
            }
            break;
        }
        case ESP_PBAC_PULL_VCARD_ENTRY_RESPONSE_EVT: {
            struct pbac_pull_vcard_entry_rsp_param *r = &param->pull_vcard_entry_rsp;
            if (p.op != OP_ENTRY) {
                break;
            }
            if (r->result != ESP_PBAC_SUCCESS) {
                finish(reply_result(r->result));
                break;
            }
            gather(r->data, r->data_len);
            if (r->final) {
                finish(BTC_OK);
            }
            break;
        }
        default:
            break;
    }
}

// ---- start and stop ----

int btc_pb_start(size_t max_bytes) {
    if (p.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (max_bytes < 1024) {
        return BTC_ERR_ARG;
    }
    memset(&p, 0, sizeof(p));
    p.max = max_bytes;
    p.book_size = p.new_missed = BTC_UNKNOWN;
    esp_pbac_register_callback(pb_cb);
    esp_err_t err = esp_pbac_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "phone book didn't start: %s", esp_err_to_name(err));
        return btc_err(err);
    }
    p.started = true;
    return BTC_OK;
}

void btc_pb_stop(void) {
    if (!p.started) {
        return;
    }
    if (p.connected) {
        esp_pbac_disconnect(p.handle);
        for (int i = 0; i < 100 && p.connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    p.started = false;
    esp_pbac_deinit();
    // Bluedroid's task may still be finishing a callback that writes the buffer.
    vTaskDelay(pdMS_TO_TICKS(50));
    btc_pb_release();
    p.connected = false;
    p.busy = false;
}

bool btc_pb_started(void) {
    return p.started;
}

int btc_pb_connect(const uint8_t *addr) {
    if (!p.started) {
        return BTC_ERR_STATE;
    }
    if (p.connected) {
        return BTC_OK;
    }
    esp_bd_addr_t a;
    if (addr) {
        memcpy(a, addr, sizeof(a));
    } else {
        btc_hf_status_t hf;
        btc_hf_status(&hf);
        if (hf.connected) {
            memcpy(a, hf.peer, sizeof(a));
        } else if (!btc_sink_peer(a)) {
            btc_addr_t paired[1];
            if (btc_radio_paired(paired, 1) < 1) {
                return BTC_ERR_STATE;
            }
            memcpy(a, paired[0], sizeof(a));
        }
    }
    BTC_LOCK();
    p.result = BTC_OK;
    BTC_UNLOCK();
    return btc_err(esp_pbac_connect(a));
}

int btc_pb_disconnect(void) {
    if (!p.started || !p.connected) {
        return BTC_ERR_STATE;
    }
    return btc_err(esp_pbac_disconnect(p.handle));
}

// ---- requests ----

// Claim the one request slot and empty the reply.
static int begin(op_t op) {
    if (!p.started || !p.connected) {
        return BTC_ERR_STATE;
    }
    BTC_LOCK();
    if (p.busy) {
        BTC_UNLOCK();
        return BTC_ERR_STATE;
    }
    p.busy = true;
    p.op = op;
    p.len = 0;
    p.truncated = false;
    p.result = BTC_OK;
    BTC_UNLOCK();
    return BTC_OK;
}

static int sent(esp_err_t err) {
    if (err != ESP_OK) {
        BTC_LOCK();
        p.busy = false;
        p.setting_path = false;
        p.op = OP_NONE;
        BTC_UNLOCK();
    }
    return btc_err(err);
}

int btc_pb_pull(const char *path, uint16_t offset, uint16_t count, uint8_t format, uint64_t properties) {
    if (path == NULL || format > 1) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_PULL);
    if (err) {
        return err;
    }
    esp_pbac_pull_phone_book_app_param_t ap = {0};
    ap.include_format = 1;
    ap.format = format;
    ap.include_max_list_count = 1;
    ap.max_list_count = count;
    if (offset) {
        ap.include_list_start_offset = 1;
        ap.list_start_offset = offset;
    }
    if (properties) {
        ap.include_property_selector = 1;
        ap.property_selector = properties;
    }
    return sent(esp_pbac_pull_phone_book(p.handle, path, &ap));
}

int btc_pb_list(const char *folder, uint16_t offset, uint16_t count, uint8_t order,
    const char *search, uint8_t search_by) {
    if (folder == NULL || order > 2 || search_by > 2 || (search && strlen(search) >= PB_NAME_MAX)) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_LIST);
    if (err) {
        return err;
    }
    esp_pbac_pull_vcard_listing_app_param_t *ap = &p.list_param;
    memset(ap, 0, sizeof(*ap));
    ap->include_order = 1;
    ap->order = order;
    ap->include_max_list_count = 1;
    ap->max_list_count = count;
    if (offset) {
        ap->include_list_start_offset = 1;
        ap->list_start_offset = offset;
    }
    if (search && search[0]) {
        strcpy(p.search, search);
        ap->include_search_value = 1;
        ap->search_value = p.search;
        ap->include_search_property = 1;
        ap->search_property = search_by;
    }
    p.setting_path = true;
    return sent(esp_pbac_set_phone_book2(p.handle, folder));
}

int btc_pb_entry(const char *folder, const char *handle, uint8_t format, uint64_t properties) {
    if (folder == NULL || handle == NULL || strlen(handle) >= PB_NAME_MAX || format > 1) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_ENTRY);
    if (err) {
        return err;
    }
    strcpy(p.name, handle);
    esp_pbac_pull_vcard_entry_app_param_t *ap = &p.entry_param;
    memset(ap, 0, sizeof(*ap));
    ap->include_format = 1;
    ap->format = format;
    if (properties) {
        ap->include_property_selector = 1;
        ap->property_selector = properties;
    }
    p.setting_path = true;
    return sent(esp_pbac_set_phone_book2(p.handle, folder));
}

size_t btc_pb_data(uint8_t *dst, size_t size) {
    BTC_LOCK();
    bool busy = p.busy;
    size_t n = p.len < size ? p.len : size;
    BTC_UNLOCK();
    if (busy) {
        return 0;
    }
    if (n) {
        memcpy(dst, p.buf, n);
    }
    return n;
}

void btc_pb_release(void) {
    BTC_LOCK();
    if (p.busy) {
        BTC_UNLOCK();
        return;
    }
    uint8_t *b = p.buf;
    p.buf = NULL;
    p.len = p.cap = 0;
    BTC_UNLOCK();
    heap_caps_free(b);
}

void btc_pb_status(btc_pb_status_t *out) {
    BTC_LOCK();
    out->started = p.started;
    out->connected = p.connected;
    memcpy(out->peer, p.peer, sizeof(btc_addr_t));
    out->repositories = p.repositories;
    out->features = p.features;
    out->busy = p.busy;
    out->result = p.result;
    out->length = p.len;
    out->truncated = p.truncated;
    out->book_size = p.book_size;
    out->new_missed = p.new_missed;
    BTC_UNLOCK();
}

uint32_t btc_pb_changes(void) {
    BTC_LOCK();
    uint32_t c = p.changes;
    p.changes = 0;
    BTC_UNLOCK();
    return c;
}

#else   // no PBAP client in this build

int btc_pb_start(size_t max_bytes) {
    (void)max_bytes;
    return BTC_ERR_UNSUPPORTED;
}
void btc_pb_stop(void) {
}
bool btc_pb_started(void) {
    return false;
}
int btc_pb_connect(const uint8_t *addr) {
    (void)addr;
    return BTC_ERR_UNSUPPORTED;
}
int btc_pb_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_pb_pull(const char *path, uint16_t offset, uint16_t count, uint8_t format, uint64_t properties) {
    (void)path; (void)offset; (void)count; (void)format; (void)properties;
    return BTC_ERR_UNSUPPORTED;
}
int btc_pb_list(const char *folder, uint16_t offset, uint16_t count, uint8_t order,
    const char *search, uint8_t search_by) {
    (void)folder; (void)offset; (void)count; (void)order; (void)search; (void)search_by;
    return BTC_ERR_UNSUPPORTED;
}
int btc_pb_entry(const char *folder, const char *handle, uint8_t format, uint64_t properties) {
    (void)folder; (void)handle; (void)format; (void)properties;
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_pb_data(uint8_t *dst, size_t size) {
    (void)dst; (void)size;
    return 0;
}
void btc_pb_release(void) {
}
void btc_pb_status(btc_pb_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->book_size = out->new_missed = BTC_UNKNOWN;
}
uint32_t btc_pb_changes(void) {
    return 0;
}

#endif
