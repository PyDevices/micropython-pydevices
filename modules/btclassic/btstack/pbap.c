// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The phone book on BTstack's PBAP client: a phone's contacts and call
// history, as vCards. See btclassic.h.
//
// One request runs at a time. A connect first reads the phone's PBAP record
// (its repositories and features), then opens the session. A listing or a
// single entry sets the phone's current folder first, then asks. The reply
// comes in pieces, gathered into one buffer from the heap that grows to
// `max_bytes`.

#include <string.h>

#include "btstack.h"
#include "classic/pbap_client.h"

#include "btc_btstack.h"

#define PB_NAME_MAX 64             // a folder, a handle or a search

typedef enum {
    OP_NONE = 0,
    OP_PULL,
    OP_SIZE,
    OP_LIST,
    OP_ENTRY,
} op_t;

static struct {
    bool started;
    bool connected;
    bool connecting;
    bool client_up;         // pbap_client_init done while the radio is on
    uint16_t cid;
    btc_addr_t peer;
    uint8_t repositories;
    uint32_t features;
    uint32_t changes;

    // The phone's PBAP record, read before the session opens.
    uint8_t attr[8];
    uint16_t attr_id;

    // The request.
    op_t op;
    bool busy;
    bool setting_path;      // waiting for the folder to be set first
    int result;
    bool truncated;
    int book_size;
    char folder[PB_NAME_MAX];
    char name[PB_NAME_MAX];     // the handle to fetch, after the folder is set
    char search[PB_NAME_MAX];
    uint16_t offset, count;
    uint8_t order, search_by, format;
    uint32_t properties;

    // The reply.
    uint8_t *buf;
    size_t len, cap, max;
} p;

static void finish(int result) {
    p.result = result;
    p.busy = false;
    p.setting_path = false;
    p.op = OP_NONE;
    p.changes |= BTC_CHANGED_PHONEBOOK;
}

// BTstack's OBEX statuses as the OBEX responses btclassic.h reports.
static int obex_result(uint8_t status) {
    switch (status) {
        case ERROR_CODE_SUCCESS: return BTC_OK;
        case OBEX_CONNECT_FAILED: return 0xc3;     // forbidden: the phone's user said no
        case OBEX_NOT_FOUND: return 0xc4;
        case OBEX_NOT_ACCEPTABLE: return 0xc6;
        case OBEX_DISCONNECTED: return BTC_ERR_STATE;
        default: return BTC_ERR_FAIL;
    }
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
        uint8_t *nb = btc_btstack_alloc(cap);
        if (nb == NULL) {
            p.truncated = true;
            return;
        }
        if (p.buf) {
            memcpy(nb, p.buf, p.len);
            btc_btstack_free(p.buf);
        }
        p.buf = nb;
        p.cap = cap;
    }
    memcpy(p.buf + p.len, data, n);
    p.len += n;
}

// The request after the folder is set.
static void after_path(void) {
    uint8_t s = ERROR_CODE_COMMAND_DISALLOWED;
    if (p.op == OP_LIST) {
        pbap_set_max_list_count(p.cid, p.count == BTC_PB_ALL ? 0 : p.count);
        pbap_set_list_start_offset(p.cid, p.offset);
        pbap_set_order(p.cid, p.order);
        pbap_set_search_value(p.cid, p.search[0] ? p.search : NULL);
        pbap_set_search_property(p.cid, p.search_by);
        s = pbap_pull_vcard_listing(p.cid, "");
    } else if (p.op == OP_ENTRY) {
        pbap_set_property_selector(p.cid, p.properties);
        pbap_set_format(p.cid, p.format);
        s = pbap_pull_vcard_entry(p.cid, p.name);
    }
    if (s != ERROR_CODE_SUCCESS) {
        finish(BTC_ERR_FAIL);
    }
}

static void on_pbap(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == PBAP_DATA_PACKET) {
        if (p.op == OP_PULL || p.op == OP_LIST || p.op == OP_ENTRY) {
            gather(packet, size);
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_PBAP_META) {
        return;
    }
    switch (hci_event_pbap_meta_get_subevent_code(packet)) {
        case PBAP_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = pbap_subevent_connection_opened_get_status(packet);
            p.connecting = false;
            if (status == ERROR_CODE_SUCCESS) {
                p.connected = true;
                p.cid = pbap_subevent_connection_opened_get_pbap_cid(packet);
                btc_log("phone book connected: repositories 0x%x, features 0x%x",
                    p.repositories, (unsigned)p.features);
            } else {
                p.result = obex_result(status);
                if (p.result == BTC_ERR_FAIL) {
                    p.result = status;      // the stack's reason, for the log
                }
                btc_log("phone book didn't connect: 0x%02x", status);
            }
            p.changes |= BTC_CHANGED_CONNECTION | BTC_CHANGED_PHONEBOOK;
            break;
        }
        case PBAP_SUBEVENT_CONNECTION_CLOSED:
            btc_log("phone book disconnected");
            p.connected = p.connecting = false;
            p.cid = 0;
            if (p.busy) {
                finish(BTC_ERR_STATE);
            }
            p.changes |= BTC_CHANGED_CONNECTION | BTC_CHANGED_PHONEBOOK;
            break;
        case PBAP_SUBEVENT_PHONEBOOK_SIZE:
            if (p.op == OP_SIZE) {
                uint8_t status = pbap_subevent_phonebook_size_get_status(packet);
                if (status == ERROR_CODE_SUCCESS) {
                    p.book_size = pbap_subevent_phonebook_size_get_phonebook_size(packet);
                }
                finish(obex_result(status));
            }
            break;
        case PBAP_SUBEVENT_OPERATION_COMPLETED: {
            int result = obex_result(pbap_subevent_operation_completed_get_status(packet));
            if (p.setting_path) {
                p.setting_path = false;
                if (result != BTC_OK) {
                    finish(result);
                } else {
                    after_path();
                }
            } else if (p.busy) {
                finish(result);
            }
            break;
        }
        case PBAP_SUBEVENT_AUTHENTICATION_REQUEST:
            // A phone that wants an OBEX password: none to give.
            if (p.connecting) {
                pbap_disconnect(p.cid);
            }
            break;
        default:
            break;
    }
}

// ---- the phone's PBAP record ----

static void on_record(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)packet_type;
    (void)channel;
    (void)size;
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE: {
            uint16_t len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            uint16_t off = sdp_event_query_attribute_byte_get_data_offset(packet);
            uint16_t id = sdp_event_query_attribute_byte_get_attribute_id(packet);
            if (off < sizeof(p.attr)) {
                p.attr[off] = sdp_event_query_attribute_byte_get_data(packet);
            }
            if (off + 1 != len || len > sizeof(p.attr)) {
                break;
            }
            // A data element: an unsigned int of 1, 2 or 4 bytes.
            uint32_t v = 0;
            uint8_t type = p.attr[0];
            int n = type == 0x08 ? 1 : type == 0x09 ? 2 : type == 0x0a ? 4 : 0;
            for (int i = 0; i < n && 1 + i < len; i++) {
                v = (v << 8) | p.attr[1 + i];
            }
            if (id == BLUETOOTH_ATTRIBUTE_SUPPORTED_REPOSITORIES) {
                p.repositories = (uint8_t)v;
            } else if (id == BLUETOOTH_ATTRIBUTE_PBAP_SUPPORTED_FEATURES) {
                p.features = v;
            }
            break;
        }
        case SDP_EVENT_QUERY_COMPLETE:
            if (p.connecting) {
                uint8_t s = pbap_connect(&on_pbap, p.peer, &p.cid);
                if (s != ERROR_CODE_SUCCESS) {
                    p.connecting = false;
                    p.result = BTC_ERR_FAIL;
                    p.changes |= BTC_CHANGED_CONNECTION | BTC_CHANGED_PHONEBOOK;
                }
            }
            break;
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
    bool client_up = p.client_up;
    memset(&p, 0, sizeof(p));
    p.client_up = client_up;
    p.max = max_bytes;
    p.book_size = BTC_UNKNOWN;
    if (!p.client_up) {
        btc_goep_init_once();
        pbap_client_init();
        p.client_up = true;
    }
    p.started = true;
    return BTC_OK;
}

void btc_pb_stop(void) {
    if (!p.started) {
        return;
    }
    if (p.connected) {
        pbap_disconnect(p.cid);
    }
    btc_pb_release();
    p.started = false;
    p.busy = false;
}

bool btc_pb_started(void) {
    return p.started;
}

// The radio went down: the stack's PBAP client goes with it.
void btc_pb_radio_stopping(void) {
    btc_pb_stop();
    btc_pb_radio_lost();
}

void btc_pb_radio_lost(void) {
    if (p.client_up) {
        pbap_client_deinit();
    }
    p.client_up = false;
    p.started = p.connected = p.connecting = p.busy = false;
    p.buf = NULL;       // the heap went with a soft reset; stopping freed it before
    p.len = p.cap = 0;
}

int btc_pb_connect(const uint8_t *addr) {
    if (!p.started) {
        return BTC_ERR_STATE;
    }
    if (p.connected || p.connecting) {
        return BTC_OK;
    }
    if (addr) {
        memcpy(p.peer, addr, sizeof(p.peer));
    } else {
        btc_hf_status_t hf;
        btc_sink_status_t sink;
        btc_hf_status(&hf);
        btc_sink_status(&sink);
        if (hf.connected) {
            memcpy(p.peer, hf.peer, sizeof(p.peer));
        } else if (sink.connected) {
            memcpy(p.peer, sink.peer, sizeof(p.peer));
        } else if (!btc_radio_last_peer(p.peer)) {
            btc_addr_t paired[1];
            if (btc_radio_paired(paired, 1) < 1) {
                return BTC_ERR_STATE;
            }
            memcpy(p.peer, paired[0], sizeof(p.peer));
        }
    }
    p.result = BTC_OK;
    p.repositories = 0;
    p.features = 0;
    p.connecting = true;
    // Read the record first; if SDP is busy, go straight to the session.
    uint8_t s = sdp_client_query_uuid16(&on_record, p.peer, BLUETOOTH_SERVICE_CLASS_PHONEBOOK_ACCESS_PSE);
    if (s != ERROR_CODE_SUCCESS) {
        s = pbap_connect(&on_pbap, p.peer, &p.cid);
    }
    if (s != ERROR_CODE_SUCCESS) {
        p.connecting = false;
        return BTC_ERR_FAIL;
    }
    return BTC_OK;
}

int btc_pb_disconnect(void) {
    if (!p.started || !p.connected) {
        return BTC_ERR_STATE;
    }
    return pbap_disconnect(p.cid) == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

// ---- requests ----

// Claim the one request slot and empty the reply.
static int begin(op_t op) {
    if (!p.started || !p.connected) {
        return BTC_ERR_STATE;
    }
    if (p.busy) {
        return BTC_ERR_STATE;
    }
    p.busy = true;
    p.op = op;
    p.len = 0;
    p.truncated = false;
    p.result = BTC_OK;
    return BTC_OK;
}

static int sent(uint8_t status) {
    if (status != ERROR_CODE_SUCCESS) {
        p.busy = false;
        p.setting_path = false;
        p.op = OP_NONE;
        return BTC_ERR_FAIL;
    }
    return BTC_OK;
}

static int set_path(const char *folder) {
    strcpy(p.folder, folder);
    p.setting_path = true;
    return sent(pbap_set_phonebook(p.cid, p.folder));
}

int btc_pb_pull(const char *path, uint16_t offset, uint16_t count, uint8_t format, uint64_t properties) {
    if (path == NULL || format > 1 || strlen(path) >= PB_NAME_MAX) {
        return BTC_ERR_ARG;
    }
    int err = begin(count == 0 ? OP_SIZE : OP_PULL);
    if (err) {
        return err;
    }
    strcpy(p.folder, path);
    if (count == 0) {
        return sent(pbap_get_phonebook_size(p.cid, p.folder));
    }
    pbap_set_max_list_count(p.cid, count == BTC_PB_ALL ? 0 : count);
    pbap_set_list_start_offset(p.cid, offset);
    pbap_set_property_selector(p.cid, (uint32_t)properties);
    pbap_set_format(p.cid, format);
    return sent(pbap_pull_phonebook(p.cid, p.folder));
}

int btc_pb_list(const char *folder, uint16_t offset, uint16_t count, uint8_t order,
    const char *search, uint8_t search_by) {
    if (folder == NULL || strlen(folder) >= PB_NAME_MAX || order > 2 || search_by > 2
        || (search && strlen(search) >= PB_NAME_MAX)) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_LIST);
    if (err) {
        return err;
    }
    p.offset = offset;
    p.count = count;
    p.order = order;
    p.search_by = search_by;
    strcpy(p.search, search ? search : "");
    return set_path(folder);
}

int btc_pb_entry(const char *folder, const char *handle, uint8_t format, uint64_t properties) {
    if (folder == NULL || handle == NULL || strlen(folder) >= PB_NAME_MAX
        || strlen(handle) >= PB_NAME_MAX || format > 1) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_ENTRY);
    if (err) {
        return err;
    }
    strcpy(p.name, handle);
    p.format = format;
    p.properties = (uint32_t)properties;
    return set_path(folder);
}

size_t btc_pb_data(uint8_t *dst, size_t size) {
    if (p.busy || p.buf == NULL) {
        return 0;
    }
    size_t n = p.len < size ? p.len : size;
    memcpy(dst, p.buf, n);
    return n;
}

void btc_pb_release(void) {
    if (p.buf) {
        btc_btstack_free(p.buf);
    }
    p.buf = NULL;
    p.len = p.cap = 0;
}

void btc_pb_status(btc_pb_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = p.started;
    out->connected = p.connected;
    memcpy(out->peer, p.peer, sizeof(out->peer));
    out->repositories = p.repositories;
    out->features = p.features;
    out->busy = p.busy || p.setting_path;
    out->result = p.result;
    out->length = (uint32_t)p.len;
    out->truncated = p.truncated;
    out->book_size = p.book_size;
    out->new_missed = BTC_UNKNOWN;      // this PBAP client doesn't read it
}

uint32_t btc_pb_changes(void) {
    uint32_t c = p.changes;
    p.changes = 0;
    return c;
}
