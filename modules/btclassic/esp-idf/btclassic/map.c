// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The message client on Bluedroid's OBEX (MAP client): a phone's texts, as
// listings and bMessages, and the phone's reports of new ones. See
// include/btclassic.h.
//
// ESP-IDF has no MAP client, but it has the GOEP client its phone book client
// runs on, and an OBEX server. Both work only on Bluedroid's own task, so
// this registers a handler there under the task's unused message-client
// slot and does all its OBEX work in it: calls from MicroPython post a
// message, and every GOEP or OBEX callback posts one too.
//
// One request runs at a time. A request in a folder first walks the phone's
// current folder there, one SETPATH a step, unless it is there already.
//
// Notifications come back the other way: the phone connects to an OBEX
// server here (the notification server, advertised in SDP) and PUTs an event
// report per change. Reports wait in a short queue for btc_msg_event().
//
// These are Bluedroid internals, so map.cmake turns this on only for the
// ESP-IDF versions it was written against.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"

#include "btc_bluedroid.h"

#if BTC_MAP

#include "esp_sdp_api.h"

#include "common/bt_target.h"
#include "osi/allocator.h"
#include "bta/bta_sys.h"
#include "stack/btm_api.h"
#include "stack/goepc_api.h"
#include "stack/obex_api.h"

static const char *TAG = "btclassic";

#define MSG_PATH_MAX    96          // a folder, "telecom/msg/inbox"
#define MSG_NAME_MAX    64          // a handle or a folder's name
#define MSG_INSTANCES   4
#define MSG_EVENTS      8
#define MSG_EVENT_MAX   1024
#define MSG_MTU         4096
#define MSG_FEATURES    0x0000007fu // notifications, browsing, upload, delete, instance info, extended reports

// The OBEX targets: the access service, and the notification service here.
static const uint8_t mas_target[16] = {
    0xbb, 0x58, 0x2b, 0x40, 0x42, 0x0c, 0x11, 0xdb, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};
static const uint8_t mns_target[16] = {
    0xbb, 0x58, 0x2b, 0x41, 0x42, 0x0c, 0x11, 0xdb, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

// Application parameter tags.
enum {
    AP_MAX_LIST_COUNT = 0x01,
    AP_LIST_START_OFFSET = 0x02,
    AP_FILTER_MESSAGE_TYPE = 0x03,
    AP_FILTER_READ_STATUS = 0x06,
    AP_ATTACHMENT = 0x0a,
    AP_TRANSPARENT = 0x0b,
    AP_RETRY = 0x0c,
    AP_NEW_MESSAGE = 0x0d,
    AP_NOTIFICATION_STATUS = 0x0e,
    AP_PARAMETER_MASK = 0x10,
    AP_FOLDER_LISTING_SIZE = 0x11,
    AP_LISTING_SIZE = 0x12,
    AP_SUBJECT_LENGTH = 0x13,
    AP_CHARSET = 0x14,
    AP_STATUS_INDICATOR = 0x17,
    AP_STATUS_VALUE = 0x18,
    AP_MSE_TIME = 0x19,
    AP_MAP_SUPPORTED_FEATURES = 0x29,
};

// What runs on Bluedroid's task.
enum {
    EV_API_CONNECT = BTA_ID_MCE << 8,
    EV_API_DISCONNECT,
    EV_API_REQUEST,
    EV_API_NOTIFY_SERVER,
    EV_API_NOTIFY_STOP,
    EV_GOEP_OPENED,
    EV_GOEP_CLOSED,
    EV_GOEP_RESPONSE,
    EV_OBEX_SERVER,
};

typedef enum {
    OP_NONE = 0,
    OP_FOLDERS,         // GET x-obex/folder-listing
    OP_LIST,            // GET x-bt/MAP-msg-listing
    OP_GET,             // GET x-bt/message
    OP_STATUS,          // PUT x-bt/messageStatus
    OP_PUSH,            // PUT x-bt/message
    OP_UPDATE,          // PUT x-bt/MAP-messageUpdate
    OP_REGISTER,        // PUT x-bt/MAP-NotificationRegistration
} op_t;

typedef struct {
    BT_HDR hdr;
    op_t op;
    char folder[MSG_PATH_MAX];      // where: the current folder for the request
    char name[MSG_NAME_MAX];        // the Name header, or ""
    uint8_t ap[64];                 // application parameters, as sent
    uint16_t ap_len;
    uint8_t *body;                  // a PUT's body (osi_malloc'd), or NULL
    size_t body_len;
} req_msg_t;

typedef struct {
    BT_HDR hdr;
    uint8_t addr[6];
    int instance;
} conn_msg_t;

typedef struct {
    BT_HDR hdr;
    uint16_t handle;
    uint8_t event;
    union {
        tGOEPC_MSG goep;
        tOBEX_MSG obex;
    };
} cb_msg_t;

typedef struct {
    bool used;
    int id;
    int psm, scn;
    uint32_t features, types;
} instance_t;

static struct {
    bool started;
    uint32_t changes;

    // The access service: found by SDP, then the GOEP link and OBEX session.
    bool searching, connecting, connected;
    btc_addr_t peer;
    int want_instance;
    instance_t inst[MSG_INSTANCES];
    int n_inst, cur;
    uint16_t goep;
    uint8_t cid[4];
    uint16_t max_tx, max_rx;
    uint32_t peer_features;

    // Where the phone's current folder is, as far as we know, and the walk.
    char cwd[MSG_PATH_MAX];
    char target[MSG_PATH_MAX];
    int walk;                       // the next step: -1 to root, then components
    req_msg_t *pending;             // the request waiting for the walk

    // The request.
    op_t op;
    bool busy;
    int result;
    bool truncated;
    int listing_size, new_message;
    char mse_time[24];
    char pushed[MSG_NAME_MAX];
    uint8_t *put_body;
    size_t put_len, put_off;

    // The reply.
    uint8_t *buf;
    size_t len, cap, max;

    // The notification server.
    bool notify_wanted, notifying;
    uint8_t scn;
    uint16_t obex_server;
    uint16_t mns_conn;
    uint32_t mns_record;
    bool record_pending;
    uint8_t *ev_buf;                // the report coming in
    size_t ev_len;
    struct {
        uint16_t len;
        uint8_t data[MSG_EVENT_MAX];
    } *events;                      // MSG_EVENTS of them, a ring
    uint8_t ev_head, ev_count;
    uint32_t ev_dropped;
} m;

static void mark(uint32_t bits) {
    BTC_LOCK();
    m.changes |= bits;
    BTC_UNLOCK();
}

static void finish(int result) {
    BTC_LOCK();
    m.result = result;
    m.busy = false;
    m.op = OP_NONE;
    BTC_UNLOCK();
    if (m.put_body) {
        osi_free(m.put_body);
        m.put_body = NULL;
    }
    if (m.pending) {
        if (m.pending->body) {
            osi_free(m.pending->body);
        }
        osi_free(m.pending);
        m.pending = NULL;
    }
    mark(BTC_CHANGED_MESSAGES);
}

// The length of an OBEX header's value, after its 3-byte head. ESP-IDF 6
// bounds the header by the packet's end; 5.5 trusts it.
static size_t hdr_value_len(BT_HDR *pkt, uint8_t *h) {
    #if ESP_IDF_VERSION_MAJOR >= 6
    uint16_t n = OBEX_GetHeaderLength(h, (uint8_t *)(pkt + 1) + pkt->offset + pkt->len);
    #else
    (void)pkt;
    uint16_t n = OBEX_GetHeaderLength(h);
    #endif
    return n > 3 ? n - 3 : 0;
}

// ---- the reply ----

static void gather(const uint8_t *data, size_t n) {
    if (n == 0 || data == NULL) {
        return;
    }
    size_t need = m.len + n;
    if (need > m.max) {
        m.truncated = true;
        need = m.max;
        n = need - m.len;
        if (n == 0) {
            return;
        }
    }
    if (need > m.cap) {
        size_t cap = m.cap ? m.cap : 4096;
        while (cap < need) {
            cap *= 2;
        }
        if (cap > m.max) {
            cap = m.max;
        }
        uint8_t *nb = heap_caps_realloc(m.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            nb = heap_caps_realloc(m.buf, cap, MALLOC_CAP_8BIT);
        }
        if (nb == NULL) {
            m.truncated = true;
            return;
        }
        BTC_LOCK();
        m.buf = nb;
        m.cap = cap;
        BTC_UNLOCK();
    }
    memcpy(m.buf + m.len, data, n);
    BTC_LOCK();
    m.len += n;
    BTC_UNLOCK();
}

// The application parameters that ride on a reply.
static void reply_params(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 2 <= n;) {
        uint8_t tag = p[i], len = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + len > n) {
            break;
        }
        BTC_LOCK();
        if ((tag == AP_LISTING_SIZE || tag == AP_FOLDER_LISTING_SIZE) && len == 2) {
            m.listing_size = (v[0] << 8) | v[1];
        } else if (tag == AP_NEW_MESSAGE && len == 1) {
            m.new_message = v[0];
        } else if (tag == AP_MSE_TIME) {
            size_t k = len < sizeof(m.mse_time) - 1 ? len : sizeof(m.mse_time) - 1;
            memcpy(m.mse_time, v, k);
            m.mse_time[k] = 0;
        }
        BTC_UNLOCK();
        i += 2 + len;
    }
}

// A Name header's UTF-16 to ASCII, for handles.
static void name_from_utf16(const uint8_t *p, size_t n, char *dst, size_t size) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < n && j + 1 < size; i += 2) {
        if (p[i] == 0 && p[i + 1] == 0) {
            break;
        }
        dst[j++] = p[i] ? '?' : (char)p[i + 1];
    }
    dst[j] = 0;
}

// ---- requests on the link ----

static uint16_t add_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0) {
        return GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_NAME, NULL, 0);
    }
    uint8_t u[2 * (MSG_PATH_MAX + 1)];
    for (size_t i = 0; i <= n; i++) {
        u[2 * i] = 0;
        u[2 * i + 1] = (uint8_t)name[i];
    }
    return GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_NAME, u, 2 * (n + 1));
}

static uint16_t add_type(const char *type) {
    return GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_TYPE, (const uint8_t *)type, strlen(type) + 1);
}

static void link_failed(void) {
    ESP_LOGW(TAG, "messages: the request couldn't be sent");
    finish(BTC_ERR_FAIL);
}

// SETPATH: to the root (name NULL, flags 2 and an empty name), or down one.
static void send_setpath(const char *name) {
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_SETPATH;
    info.flags = 0x02;                  // don't create
    uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, m.max_tx);
    ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_CONNECTION_ID, m.cid, 4);
    ret |= add_name(name ? name : "");
    ret |= GOEPC_SendRequest(m.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
    }
}

static void send_empty_get(void) {
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_GET_FINAL;
    uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, m.max_tx);
    ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_CONNECTION_ID, m.cid, 4);
    ret |= GOEPC_SendRequest(m.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
    }
}

static const char *op_type(op_t op) {
    switch (op) {
        case OP_FOLDERS:
            return "x-obex/folder-listing";
        case OP_LIST:
            return "x-bt/MAP-msg-listing";
        case OP_GET:
        case OP_PUSH:
            return "x-bt/message";
        case OP_STATUS:
            return "x-bt/messageStatus";
        case OP_UPDATE:
            return "x-bt/MAP-messageUpdate";
        case OP_REGISTER:
            return "x-bt/MAP-NotificationRegistration";
        default:
            return "";
    }
}

// The next PUT packet of the body: as much as fits, final with the last.
static void send_put_piece(req_msg_t *r, bool first) {
    tOBEX_PARSE_INFO info = {0};
    size_t room = m.max_tx > 400 ? m.max_tx - 300 : 100;    // headers ride on the first
    size_t left = m.put_len - m.put_off;
    bool last = left <= room;
    info.opcode = last ? OBEX_OPCODE_PUT_FINAL : OBEX_OPCODE_PUT;
    uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, m.max_tx);
    ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_CONNECTION_ID, m.cid, 4);
    if (first) {
        ret |= add_type(op_type(m.op));
        if (r->op != OP_UPDATE && r->op != OP_REGISTER) {
            ret |= add_name(r->name);
        }
        if (r->ap_len) {
            ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_APP_PARAM, r->ap, r->ap_len);
        }
    }
    size_t n = last ? left : room;
    ret |= GOEPC_RequestAddHeader(m.goep, last ? OBEX_HEADER_ID_END_OF_BODY : OBEX_HEADER_ID_BODY,
        m.put_body + m.put_off, n);
    m.put_off += n;
    ret |= GOEPC_SendRequest(m.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
    }
}

// The request itself, now that the phone is in its folder.
static void send_request(req_msg_t *r) {
    BTC_LOCK();
    m.op = r->op;
    BTC_UNLOCK();
    if (r->op == OP_FOLDERS || r->op == OP_LIST || r->op == OP_GET) {
        tOBEX_PARSE_INFO info = {0};
        info.opcode = OBEX_OPCODE_GET_FINAL;
        uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, m.max_tx);
        ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_CONNECTION_ID, m.cid, 4);
        ret |= GOEPC_RequestSetSRM(m.goep, TRUE, FALSE);
        ret |= add_type(op_type(r->op));
        if (r->op != OP_FOLDERS) {
            ret |= add_name(r->name);
        }
        if (r->ap_len) {
            ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_APP_PARAM, r->ap, r->ap_len);
        }
        ret |= GOEPC_SendRequest(m.goep);
        if (ret != GOEP_SUCCESS) {
            link_failed();
        }
    } else {
        // A PUT: the body (or the "0" filler the spec asks for) in pieces.
        if (r->body) {
            m.put_body = r->body;
            m.put_len = r->body_len;
            r->body = NULL;
        } else {
            m.put_body = osi_malloc(1);
            m.put_body[0] = '0';
            m.put_len = 1;
        }
        m.put_off = 0;
        send_put_piece(r, true);
    }
}

// The walk to the request's folder: the root, then a step per component.
static void walk_next(void) {
    req_msg_t *r = m.pending;
    if (m.walk < 0) {
        m.cwd[0] = 0;
        m.walk = 0;
        send_setpath(NULL);
        return;
    }
    // The component at index m.walk of the target path.
    const char *s = m.target;
    for (int i = 0; i < m.walk && s; i++) {
        s = strchr(s, '/');
        if (s) {
            s++;
        }
    }
    if (s == NULL || *s == 0) {
        strcpy(m.cwd, m.target);
        send_request(r);
        return;
    }
    char part[MSG_NAME_MAX];
    const char *e = strchr(s, '/');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= sizeof(part)) {
        n = sizeof(part) - 1;
    }
    memcpy(part, s, n);
    part[n] = 0;
    m.walk++;
    send_setpath(part);
}

static void start_request(req_msg_t *r) {
    if (!m.connected) {
        osi_free(r->body);
        finish(BTC_ERR_STATE);
        return;
    }
    // Keep the request for the walk and its PUT pieces; finish() frees it.
    req_msg_t *keep = osi_malloc(sizeof(*keep));
    memcpy(keep, r, sizeof(*keep));
    r->body = NULL;
    m.pending = keep;
    if (strcmp(m.cwd, keep->folder) == 0) {
        send_request(keep);
    } else {
        strcpy(m.target, keep->folder);
        m.walk = -1;
        walk_next();
    }
}

// ---- responses ----

static void on_response(cb_msg_t *c) {
    BT_HDR *pkt = c->goep.response.pkt;
    uint8_t opcode = c->goep.response.opcode;
    tOBEX_PARSE_INFO info;
    OBEX_ParseResponse(pkt, opcode, &info);
    uint8_t rc = info.response_code & 0x7f;
    uint8_t *h;

    if (opcode == OBEX_OPCODE_CONNECT) {
        if (rc != OBEX_RESPONSE_CODE_OK) {
            ESP_LOGW(TAG, "messages: the phone refused the session: 0x%02x", rc);
            GOEPC_Close(m.goep);
            m.goep = 0;
            BTC_LOCK();
            m.connecting = false;
            m.result = rc | 0x80;
            BTC_UNLOCK();
            mark(BTC_CHANGED_CONNECTION);
        } else {
            if (info.max_packet_length >= 255 && info.max_packet_length < m.max_tx) {
                m.max_tx = info.max_packet_length;
            }
            while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                if (*h == OBEX_HEADER_ID_CONNECTION_ID) {
                    memcpy(m.cid, h + 1, 4);
                }
            }
            BTC_LOCK();
            m.connecting = false;
            m.connected = true;
            m.cwd[0] = 0;
            BTC_UNLOCK();
            ESP_LOGI(TAG, "messages connected: instance %d", m.inst[m.cur].id);
            mark(BTC_CHANGED_CONNECTION);
        }
    } else if (opcode == OBEX_OPCODE_DISCONNECT) {
        GOEPC_Close(m.goep);
        m.goep = 0;
        BTC_LOCK();
        m.connected = false;
        BTC_UNLOCK();
        mark(BTC_CHANGED_CONNECTION);
    } else if (opcode == OBEX_OPCODE_SETPATH) {
        if (m.pending == NULL) {
            // nothing waits
        } else if (rc != OBEX_RESPONSE_CODE_OK) {
            m.cwd[0] = 0x01;            // unknown: walk from the root next time
            finish(rc | 0x80);
        } else {
            walk_next();
        }
    } else if (opcode == OBEX_OPCODE_GET_FINAL) {
        if (rc != OBEX_RESPONSE_CODE_OK && rc != OBEX_RESPONSE_CODE_CONTINUE) {
            finish(rc | 0x80);
        } else {
            while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                if (*h == OBEX_HEADER_ID_BODY || *h == OBEX_HEADER_ID_END_OF_BODY) {
                    gather(h + 3, hdr_value_len(pkt, h));
                } else if (*h == OBEX_HEADER_ID_APP_PARAM) {
                    reply_params(h + 3, hdr_value_len(pkt, h));
                }
            }
            if (rc == OBEX_RESPONSE_CODE_OK) {
                finish(BTC_OK);
            } else if (!c->goep.response.srm_en || c->goep.response.srm_wait) {
                send_empty_get();
            }
        }
    } else if (opcode == OBEX_OPCODE_PUT || opcode == OBEX_OPCODE_PUT_FINAL) {
        if (rc == OBEX_RESPONSE_CODE_CONTINUE && m.put_off < m.put_len) {
            send_put_piece(m.pending, false);
        } else if (rc != OBEX_RESPONSE_CODE_OK) {
            finish(rc | 0x80);
        } else {
            while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                if (*h == OBEX_HEADER_ID_NAME) {
                    BTC_LOCK();
                    name_from_utf16(h + 3, hdr_value_len(pkt, h), m.pushed, sizeof(m.pushed));
                    BTC_UNLOCK();
                }
            }
            if (m.op == OP_REGISTER) {
                BTC_LOCK();
                m.notifying = m.notify_wanted;
                BTC_UNLOCK();
            }
            finish(BTC_OK);
        }
    }
    osi_free(pkt);
}

// ---- the GOEP link ----

static void goep_cb(UINT16 handle, UINT8 event, tGOEPC_MSG *msg) {
    cb_msg_t *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        if (event == GOEPC_RESPONSE_EVT) {
            osi_free(msg->response.pkt);
        }
        return;
    }
    c->handle = handle;
    c->event = event;
    if (msg) {
        c->goep = *msg;
    }
    switch (event) {
        case GOEPC_OPENED_EVT:
            c->hdr.event = EV_GOEP_OPENED;
            break;
        case GOEPC_CLOSED_EVT:
            c->hdr.event = EV_GOEP_CLOSED;
            break;
        case GOEPC_RESPONSE_EVT:
            c->hdr.event = EV_GOEP_RESPONSE;
            break;
        default:
            osi_free(c);
            return;
    }
    bta_sys_sendmsg(c);
}

static void on_opened(cb_msg_t *c) {
    if (c->goep.opened.peer_mtu && c->goep.opened.peer_mtu < m.max_tx) {
        m.max_tx = c->goep.opened.peer_mtu;
    }
    if (c->goep.opened.our_mtu && c->goep.opened.our_mtu < m.max_rx) {
        m.max_rx = c->goep.opened.our_mtu;
    }
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_CONNECT;
    info.obex_version_number = OBEX_VERSION_NUMBER;
    info.max_packet_length = m.max_rx;
    uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, BT_SMALL_BUFFER_SIZE);
    ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_TARGET, mas_target, 16);
    if (m.inst[m.cur].features) {
        uint8_t ap[6] = { AP_MAP_SUPPORTED_FEATURES, 4,
                          (uint8_t)(MSG_FEATURES >> 24), (uint8_t)(MSG_FEATURES >> 16),
                          (uint8_t)(MSG_FEATURES >> 8), (uint8_t)MSG_FEATURES };
        ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_APP_PARAM, ap, 6);
    }
    ret |= GOEPC_SendRequest(m.goep);
    if (ret != GOEP_SUCCESS) {
        GOEPC_Close(m.goep);
        m.goep = 0;
        BTC_LOCK();
        m.connecting = false;
        m.result = BTC_ERR_FAIL;
        BTC_UNLOCK();
        mark(BTC_CHANGED_CONNECTION);
    }
}

static void on_closed(void) {
    m.goep = 0;
    bool was = m.connected;
    BTC_LOCK();
    m.connected = false;
    m.connecting = false;
    m.notifying = false;
    if (!was && m.result == BTC_OK) {
        m.result = BTC_ERR_FAIL;
    }
    BTC_UNLOCK();
    if (m.busy) {
        finish(BTC_ERR_STATE);
    }
    ESP_LOGI(TAG, "messages disconnected");
    mark(BTC_CHANGED_CONNECTION);
}

// Open the link to the chosen instance, over L2CAP where the phone offers it.
static void open_instance(void) {
    instance_t *in = &m.inst[m.cur];
    tOBEX_SVR_INFO svr = {0};
    if (in->psm > 0) {
        svr.tl = OBEX_OVER_L2CAP;
        svr.l2cap.psm = in->psm;
        svr.l2cap.sec_mask = BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT;
        svr.l2cap.pref_mtu = 0;
        memcpy(svr.l2cap.addr, m.peer, 6);
    } else {
        svr.tl = OBEX_OVER_RFCOMM;
        svr.rfcomm.scn = in->scn;
        svr.rfcomm.sec_mask = BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT;
        svr.rfcomm.pref_mtu = 0;
        memcpy(svr.rfcomm.addr, m.peer, 6);
    }
    m.max_tx = m.max_rx = MSG_MTU;
    if (GOEPC_Open(&svr, goep_cb, &m.goep) != GOEP_SUCCESS) {
        BTC_LOCK();
        m.connecting = false;
        m.result = BTC_ERR_FAIL;
        BTC_UNLOCK();
        mark(BTC_CHANGED_CONNECTION);
    }
}

// ---- SDP: the phone's message services, and the record for ours ----

static void sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param) {
    if (event == ESP_SDP_SEARCH_COMP_EVT && m.searching) {
        int n = 0;
        if (param->search.status == ESP_SDP_SUCCESS) {
            for (int i = 0; i < param->search.record_count && n < MSG_INSTANCES; i++) {
                esp_bluetooth_sdp_record_t *rec = &param->search.records[i];
                if (rec->hdr.type != ESP_SDP_TYPE_MAP_MAS) {
                    continue;
                }
                instance_t *in = &m.inst[n++];
                in->used = true;
                in->id = rec->mas.mas_instance_id;
                in->psm = rec->hdr.l2cap_psm;
                in->scn = rec->hdr.rfcomm_channel_number;
                in->features = rec->mas.supported_features;
                in->types = rec->mas.supported_message_types;
            }
        }
        // On to Bluedroid's task to connect.
        conn_msg_t *c = osi_malloc(sizeof(*c));
        if (c) {
            memset(c, 0, sizeof(*c));
            c->hdr.event = EV_API_CONNECT;
            c->instance = -2;           // the search finished
            BTC_LOCK();
            m.n_inst = n;
            BTC_UNLOCK();
            bta_sys_sendmsg(c);
        }
    } else if (event == ESP_SDP_CREATE_RECORD_COMP_EVT && m.record_pending) {
        // SDP is shared: a completion is ours only while ours is pending.
        BTC_LOCK();
        m.record_pending = false;
        if (param->create_record.status == ESP_SDP_SUCCESS) {
            m.mns_record = param->create_record.record_handle;
        }
        BTC_UNLOCK();
        if (!m.mns_record) {
            ESP_LOGW(TAG, "messages: the notification record wasn't made (status %d)", param->create_record.status);
        }
    }
}

static void on_connect(conn_msg_t *c) {
    if (c->instance != -2) {
        // From the caller: search first.
        if (m.connected || m.connecting) {
            return;
        }
        memcpy(m.peer, c->addr, 6);
        m.want_instance = c->instance;
        m.connecting = true;
        m.searching = true;
        memset(m.inst, 0, sizeof(m.inst));
        if (esp_sdp_search_record(m.peer, (esp_bt_uuid_t){.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_SDP_UUID_MAP_MAS}) != ESP_OK) {
            BTC_LOCK();
            m.connecting = m.searching = false;
            m.result = BTC_ERR_FAIL;
            BTC_UNLOCK();
            mark(BTC_CHANGED_CONNECTION);
        }
        return;
    }
    m.searching = false;
    int pick = -1;
    for (int i = 0; i < m.n_inst; i++) {
        if (m.want_instance < 0 ? (m.inst[i].types & 0x06) != 0 : m.inst[i].id == m.want_instance) {
            pick = i;
            break;
        }
    }
    if (pick < 0 && m.want_instance < 0 && m.n_inst > 0) {
        pick = 0;
    }
    if (pick < 0) {
        ESP_LOGW(TAG, "messages: the phone offers no message access (%d services)", m.n_inst);
        BTC_LOCK();
        m.connecting = false;
        m.result = BTC_ERR_UNSUPPORTED;
        BTC_UNLOCK();
        mark(BTC_CHANGED_CONNECTION);
        return;
    }
    m.cur = pick;
    open_instance();
}

// ---- the notification server ----

static void obex_server_cb(UINT16 handle, UINT8 event, tOBEX_MSG *msg) {
    cb_msg_t *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        if (event == OBEX_DATA_EVT) {
            osi_free(msg->data.pkt);
        }
        return;
    }
    c->hdr.event = EV_OBEX_SERVER;
    c->handle = handle;
    c->event = event;
    if (msg) {
        c->obex = *msg;
    }
    bta_sys_sendmsg(c);
}

static void respond(uint16_t conn, uint8_t opcode, uint8_t code, bool connect_reply) {
    tOBEX_PARSE_INFO info = {0};
    info.opcode = opcode;
    info.response_code = code | OBEX_FINAL_BIT_MASK;
    if (connect_reply) {
        info.obex_version_number = OBEX_VERSION_NUMBER;
        info.max_packet_length = MSG_MTU;
    }
    BT_HDR *pkt = NULL;
    if (OBEX_BuildResponse(&info, 256, &pkt) != OBEX_SUCCESS) {
        return;
    }
    if (connect_reply && code == OBEX_RESPONSE_CODE_OK) {
        static const uint8_t cid[4] = {0, 0, 0, 1};
        OBEX_AppendHeaderRaw(pkt, OBEX_HEADER_ID_WHO, mns_target, 16);
        OBEX_AppendHeaderRaw(pkt, OBEX_HEADER_ID_CONNECTION_ID, cid, 4);
    }
    OBEX_SendPacket(conn, pkt);
}

static void queue_event(void) {
    if (m.ev_len == 0 || m.events == NULL) {
        return;
    }
    BTC_LOCK();
    if (m.ev_count == MSG_EVENTS) {
        m.ev_head = (m.ev_head + 1) % MSG_EVENTS;
        m.ev_count--;
        m.ev_dropped++;
    }
    int slot = (m.ev_head + m.ev_count) % MSG_EVENTS;
    size_t n = m.ev_len < MSG_EVENT_MAX ? m.ev_len : MSG_EVENT_MAX;
    memcpy(m.events[slot].data, m.ev_buf, n);
    m.events[slot].len = n;
    m.ev_count++;
    BTC_UNLOCK();
    mark(BTC_CHANGED_NOTIFY);
}

static void on_server(cb_msg_t *c) {
    switch (c->event) {
        case OBEX_CONN_INCOME_EVT:
            m.mns_conn = c->handle;
            ESP_LOGI(TAG, "messages: the phone opened notifications");
            break;
        case OBEX_DISCONNECT_EVT:
            if (c->handle == m.mns_conn) {
                m.mns_conn = 0;
            }
            break;
        case OBEX_DATA_EVT: {
            BT_HDR *pkt = c->obex.data.pkt;
            tOBEX_PARSE_INFO info = {0};
            OBEX_ParseRequest(pkt, &info);
            uint8_t op = info.opcode;
            uint8_t *h;
            if (op == OBEX_OPCODE_CONNECT) {
                bool ours = false;
                while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                    if (*h == OBEX_HEADER_ID_TARGET && hdr_value_len(pkt, h) == 16 && memcmp(h + 3, mns_target, 16) == 0) {
                        ours = true;
                    }
                }
                respond(c->handle, op, ours ? OBEX_RESPONSE_CODE_OK : OBEX_RESPONSE_CODE_NOT_ACCEPTABLE, true);
            } else if (op == OBEX_OPCODE_PUT || op == OBEX_OPCODE_PUT_FINAL) {
                if (m.ev_buf == NULL) {
                    m.ev_buf = osi_malloc(MSG_EVENT_MAX);
                    m.ev_len = 0;
                }
                while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                    if ((*h == OBEX_HEADER_ID_BODY || *h == OBEX_HEADER_ID_END_OF_BODY) && m.ev_buf) {
                        size_t n = hdr_value_len(pkt, h);
                        if (m.ev_len + n > MSG_EVENT_MAX) {
                            n = MSG_EVENT_MAX - m.ev_len;
                        }
                        memcpy(m.ev_buf + m.ev_len, h + 3, n);
                        m.ev_len += n;
                    }
                }
                if (op == OBEX_OPCODE_PUT_FINAL) {
                    queue_event();
                    osi_free(m.ev_buf);
                    m.ev_buf = NULL;
                    m.ev_len = 0;
                    respond(c->handle, op, OBEX_RESPONSE_CODE_OK, false);
                } else {
                    respond(c->handle, op, OBEX_RESPONSE_CODE_CONTINUE, false);
                }
            } else if (op == OBEX_OPCODE_DISCONNECT) {
                respond(c->handle, op, OBEX_RESPONSE_CODE_OK, false);
            } else if (op == OBEX_OPCODE_ABORT) {
                osi_free(m.ev_buf);
                m.ev_buf = NULL;
                m.ev_len = 0;
                respond(c->handle, op, OBEX_RESPONSE_CODE_OK, false);
            } else {
                respond(c->handle, op, OBEX_RESPONSE_CODE_NOT_IMPLEMENTED, false);
            }
            osi_free(pkt);
            break;
        }
        default:
            break;
    }
}

// Listen for the phone's notifications: an OBEX server on an RFCOMM
// channel, and the SDP record that tells the phone where.
static void on_notify_server(void) {
    if (m.obex_server) {
        return;
    }
    m.scn = BTM_AllocateSCN();
    if (m.scn == 0) {
        ESP_LOGW(TAG, "messages: no RFCOMM channel for notifications");
        return;
    }
    tOBEX_SVR_INFO svr = {0};
    svr.tl = OBEX_OVER_RFCOMM;
    svr.rfcomm.scn = m.scn;
    svr.rfcomm.sec_mask = BTM_SEC_IN_AUTHENTICATE | BTM_SEC_IN_ENCRYPT;
    svr.rfcomm.pref_mtu = 0;
    if (OBEX_RegisterServer(&svr, obex_server_cb, &m.obex_server) != OBEX_SUCCESS) {
        ESP_LOGW(TAG, "messages: the notification server didn't start");
        BTM_FreeSCN(m.scn);
        m.scn = 0;
        m.obex_server = 0;
        return;
    }
    esp_bluetooth_sdp_record_t rec = {0};
    rec.mns.hdr.type = ESP_SDP_TYPE_MAP_MNS;
    rec.mns.hdr.service_name = "MAP MNS";
    rec.mns.hdr.service_name_length = sizeof("MAP MNS");  // with its NUL, which esp_sdp checks
    rec.mns.hdr.rfcomm_channel_number = m.scn;
    rec.mns.hdr.l2cap_psm = -1;
    rec.mns.hdr.profile_version = 0x0104;
    rec.mns.supported_features = MSG_FEATURES;
    m.record_pending = true;
    if (esp_sdp_create_record(&rec) != ESP_OK) {
        m.record_pending = false;
        ESP_LOGW(TAG, "messages: the notification record wasn't made");
    }
}

// Close the notification link, the server and its channel.
static void on_notify_stop(void) {
    if (m.mns_conn) {
        OBEX_RemoveConn(m.mns_conn);
        m.mns_conn = 0;
    }
    if (m.obex_server) {
        OBEX_DeregisterServer(m.obex_server);
        m.obex_server = 0;
    }
    if (m.scn) {
        BTM_FreeSCN(m.scn);
        m.scn = 0;
    }
}

// ---- Bluedroid's task ----

static BOOLEAN task_handler(BT_HDR *msg) {
    switch (msg->event) {
        case EV_API_CONNECT:
            on_connect((conn_msg_t *)msg);
            break;
        case EV_API_DISCONNECT:
            if (m.connected) {
                tOBEX_PARSE_INFO info = {0};
                info.opcode = OBEX_OPCODE_DISCONNECT;
                uint16_t ret = GOEPC_PrepareRequest(m.goep, &info, BT_SMALL_BUFFER_SIZE);
                ret |= GOEPC_RequestAddHeader(m.goep, OBEX_HEADER_ID_CONNECTION_ID, m.cid, 4);
                ret |= GOEPC_SendRequest(m.goep);
                if (ret != GOEP_SUCCESS) {
                    GOEPC_Close(m.goep);
                    on_closed();
                }
            } else if (m.goep) {
                GOEPC_Close(m.goep);
                on_closed();
            }
            break;
        case EV_API_REQUEST:
            start_request((req_msg_t *)msg);
            break;
        case EV_API_NOTIFY_SERVER:
            on_notify_server();
            break;
        case EV_API_NOTIFY_STOP:
            on_notify_stop();
            break;
        case EV_GOEP_OPENED:
            on_opened((cb_msg_t *)msg);
            break;
        case EV_GOEP_CLOSED:
            on_closed();
            break;
        case EV_GOEP_RESPONSE:
            on_response((cb_msg_t *)msg);
            break;
        case EV_OBEX_SERVER:
            on_server((cb_msg_t *)msg);
            break;
        default:
            break;
    }
    return TRUE;
}

static const tBTA_SYS_REG task_reg = { task_handler, NULL };

// ---- the API ----

int btc_msg_start(size_t max_bytes) {
    if (m.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (max_bytes < 1024) {
        return BTC_ERR_ARG;
    }
    memset(&m, 0, sizeof(m));
    m.max = max_bytes;
    m.listing_size = m.new_message = BTC_UNKNOWN;
    m.events = heap_caps_malloc(sizeof(*m.events) * MSG_EVENTS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m.events == NULL) {
        m.events = heap_caps_malloc(sizeof(*m.events) * MSG_EVENTS, MALLOC_CAP_8BIT);
    }
    if (m.events == NULL) {
        return BTC_ERR_NOMEM;
    }
    esp_err_t err = btc_sdp_open(sdp_cb);    // shared with HIDDevice
    if (err != ESP_OK) {
        heap_caps_free(m.events);
        m.events = NULL;
        ESP_LOGW(TAG, "messages: SDP didn't start: %s", esp_err_to_name(err));
        return btc_err(err);
    }
    bta_sys_register(BTA_ID_MCE, &task_reg);
    m.started = true;
    return BTC_OK;
}

void btc_msg_stop(void) {
    if (!m.started) {
        return;
    }
    if (m.connected || m.goep) {
        btc_msg_disconnect();
        for (int i = 0; i < 100 && (m.connected || m.goep); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (m.mns_record) {
        esp_sdp_remove_record(m.mns_record);
        m.mns_record = 0;
    }
    if (m.obex_server || m.mns_conn) {
        // Unbound on Bluedroid's task, which owns them; a second Messages
        // would otherwise find no server slot left.
        BT_HDR *c = osi_malloc(sizeof(*c));
        if (c) {
            c->event = EV_API_NOTIFY_STOP;
            bta_sys_sendmsg(c);
            for (int i = 0; i < 50 && (m.obex_server || m.mns_conn); i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    m.started = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    btc_sdp_close(sdp_cb);
    btc_msg_release();
    heap_caps_free(m.events);
    m.events = NULL;
}

bool btc_msg_started(void) {
    return m.started;
}

// Start the notification server and wait for its SDP record.
static int notify_server_up(void) {
    BT_HDR *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        return BTC_ERR_NOMEM;
    }
    c->event = EV_API_NOTIFY_SERVER;
    bta_sys_sendmsg(c);
    for (int i = 0; i < 50 && (m.record_pending || !m.obex_server); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Without its record the phone can't find the server, and never calls.
    return m.obex_server && m.mns_record ? BTC_OK : BTC_ERR_FAIL;
}

int btc_msg_connect(const uint8_t *addr, int instance) {
    if (!m.started) {
        return BTC_ERR_STATE;
    }
    if (m.connected) {
        return BTC_OK;
    }
    // Android looks for the notification record once, as the session opens,
    // and drops later registrations if it found none: so the record goes up
    // first. Without it everything but notify() still works.
    if ((!m.obex_server || !m.mns_record) && notify_server_up() != BTC_OK) {
        ESP_LOGW(TAG, "messages: no notification server; notify() will fail");
    }
    btc_addr_t a;
    if (addr) {
        memcpy(a, addr, 6);
    } else {
        btc_hf_status_t hf;
        btc_hf_status(&hf);
        if (hf.connected) {
            memcpy(a, hf.peer, 6);
        } else if (!btc_sink_peer(a)) {
            btc_addr_t paired[1];
            if (btc_radio_paired(paired, 1) < 1) {
                return BTC_ERR_STATE;
            }
            memcpy(a, paired[0], 6);
        }
    }
    conn_msg_t *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        return BTC_ERR_NOMEM;
    }
    memset(c, 0, sizeof(*c));
    c->hdr.event = EV_API_CONNECT;
    memcpy(c->addr, a, 6);
    c->instance = instance < 0 ? -1 : instance;
    BTC_LOCK();
    m.result = BTC_OK;
    BTC_UNLOCK();
    bta_sys_sendmsg(c);
    return BTC_OK;
}

int btc_msg_disconnect(void) {
    if (!m.started || (!m.connected && !m.goep)) {
        return BTC_ERR_STATE;
    }
    BT_HDR *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        return BTC_ERR_NOMEM;
    }
    c->event = EV_API_DISCONNECT;
    bta_sys_sendmsg(c);
    return BTC_OK;
}

// Claim the request slot and post the request to Bluedroid's task.
static int post(req_msg_t *r) {
    if (!m.started || !m.connected) {
        osi_free(r->body);
        osi_free(r);
        return BTC_ERR_STATE;
    }
    BTC_LOCK();
    if (m.busy) {
        BTC_UNLOCK();
        osi_free(r->body);
        osi_free(r);
        return BTC_ERR_STATE;
    }
    m.busy = true;
    m.len = 0;
    m.truncated = false;
    m.result = BTC_OK;
    m.pushed[0] = 0;
    BTC_UNLOCK();
    r->hdr.event = EV_API_REQUEST;
    bta_sys_sendmsg(r);
    return BTC_OK;
}

static req_msg_t *new_req(op_t op, const char *folder, const char *name) {
    if (folder == NULL || strlen(folder) >= MSG_PATH_MAX || (name && strlen(name) >= MSG_NAME_MAX)) {
        return NULL;
    }
    req_msg_t *r = osi_malloc(sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->op = op;
    strcpy(r->folder, folder);
    if (name) {
        strcpy(r->name, name);
    }
    return r;
}

static void ap8(req_msg_t *r, uint8_t tag, uint8_t v) {
    r->ap[r->ap_len++] = tag;
    r->ap[r->ap_len++] = 1;
    r->ap[r->ap_len++] = v;
}

static void ap16(req_msg_t *r, uint8_t tag, uint16_t v) {
    r->ap[r->ap_len++] = tag;
    r->ap[r->ap_len++] = 2;
    r->ap[r->ap_len++] = v >> 8;
    r->ap[r->ap_len++] = v & 0xff;
}

static void ap32(req_msg_t *r, uint8_t tag, uint32_t v) {
    r->ap[r->ap_len++] = tag;
    r->ap[r->ap_len++] = 4;
    r->ap[r->ap_len++] = v >> 24;
    r->ap[r->ap_len++] = v >> 16;
    r->ap[r->ap_len++] = v >> 8;
    r->ap[r->ap_len++] = v & 0xff;
}

int btc_msg_folders(const char *folder, uint16_t offset, uint16_t count) {
    req_msg_t *r = new_req(OP_FOLDERS, folder, NULL);
    if (r == NULL) {
        return BTC_ERR_ARG;
    }
    ap16(r, AP_MAX_LIST_COUNT, count);
    if (offset) {
        ap16(r, AP_LIST_START_OFFSET, offset);
    }
    return post(r);
}

int btc_msg_list(const char *folder, const char *name, uint16_t offset, uint16_t count,
    uint8_t read_filter, uint8_t type_filter, uint32_t parameter_mask, uint8_t subject_length) {
    req_msg_t *r = new_req(OP_LIST, folder, name);
    if (r == NULL) {
        return BTC_ERR_ARG;
    }
    ap16(r, AP_MAX_LIST_COUNT, count);
    if (offset) {
        ap16(r, AP_LIST_START_OFFSET, offset);
    }
    if (read_filter) {
        ap8(r, AP_FILTER_READ_STATUS, read_filter);
    }
    if (type_filter) {
        ap8(r, AP_FILTER_MESSAGE_TYPE, type_filter);
    }
    if (parameter_mask) {
        ap32(r, AP_PARAMETER_MASK, parameter_mask);
    }
    if (subject_length) {
        ap8(r, AP_SUBJECT_LENGTH, subject_length);
    }
    return post(r);
}

int btc_msg_get(const char *handle, bool attachments, bool utf8) {
    req_msg_t *r = new_req(OP_GET, "", handle);
    if (r == NULL || handle == NULL) {
        osi_free(r);
        return BTC_ERR_ARG;
    }
    // A message is fetched by handle from anywhere: keep the folder as is.
    strcpy(r->folder, m.cwd[0] == 0x01 ? "" : m.cwd);
    ap8(r, AP_ATTACHMENT, attachments ? 1 : 0);
    ap8(r, AP_CHARSET, utf8 ? 1 : 0);
    return post(r);
}

int btc_msg_set_status(const char *handle, uint8_t indicator, bool value) {
    req_msg_t *r = new_req(OP_STATUS, "", handle);
    if (r == NULL || handle == NULL || indicator > 1) {
        osi_free(r);
        return BTC_ERR_ARG;
    }
    strcpy(r->folder, m.cwd[0] == 0x01 ? "" : m.cwd);
    ap8(r, AP_STATUS_INDICATOR, indicator);
    ap8(r, AP_STATUS_VALUE, value ? 1 : 0);
    return post(r);
}

int btc_msg_push(const char *folder, const char *name, const uint8_t *bmsg, size_t len, bool utf8, bool retry) {
    if (bmsg == NULL || len == 0) {
        return BTC_ERR_ARG;
    }
    req_msg_t *r = new_req(OP_PUSH, folder, name);
    if (r == NULL) {
        return BTC_ERR_ARG;
    }
    r->body = osi_malloc(len);
    if (r->body == NULL) {
        osi_free(r);
        return BTC_ERR_NOMEM;
    }
    memcpy(r->body, bmsg, len);
    r->body_len = len;
    ap8(r, AP_CHARSET, utf8 ? 1 : 0);
    ap8(r, AP_RETRY, retry ? 1 : 0);
    return post(r);
}

int btc_msg_update_inbox(void) {
    req_msg_t *r = new_req(OP_UPDATE, "", NULL);
    if (r == NULL) {
        return BTC_ERR_NOMEM;
    }
    strcpy(r->folder, m.cwd[0] == 0x01 ? "" : m.cwd);
    return post(r);
}

int btc_msg_notify(bool on) {
    if (!m.started || !m.connected) {
        return BTC_ERR_STATE;
    }
    if (on && (!m.obex_server || !m.mns_record)) {
        int err = notify_server_up();
        if (err != BTC_OK) {
            return err;
        }
    }
    req_msg_t *r = new_req(OP_REGISTER, "", NULL);
    if (r == NULL) {
        return BTC_ERR_NOMEM;
    }
    strcpy(r->folder, m.cwd[0] == 0x01 ? "" : m.cwd);
    ap8(r, AP_NOTIFICATION_STATUS, on ? 1 : 0);
    BTC_LOCK();
    m.notify_wanted = on;
    BTC_UNLOCK();
    return post(r);
}

size_t btc_msg_data(uint8_t *dst, size_t size) {
    BTC_LOCK();
    bool busy = m.busy;
    size_t n = m.len < size ? m.len : size;
    BTC_UNLOCK();
    if (busy) {
        return 0;
    }
    if (n) {
        memcpy(dst, m.buf, n);
    }
    return n;
}

void btc_msg_release(void) {
    BTC_LOCK();
    if (m.busy) {
        BTC_UNLOCK();
        return;
    }
    uint8_t *b = m.buf;
    m.buf = NULL;
    m.len = m.cap = 0;
    BTC_UNLOCK();
    heap_caps_free(b);
}

size_t btc_msg_event(uint8_t *dst, size_t size) {
    size_t n = 0;
    BTC_LOCK();
    if (m.events && m.ev_count) {
        n = m.events[m.ev_head].len;
        if (n > size) {
            n = size;
        }
        memcpy(dst, m.events[m.ev_head].data, n);
        m.ev_head = (m.ev_head + 1) % MSG_EVENTS;
        m.ev_count--;
    }
    BTC_UNLOCK();
    return n;
}

void btc_msg_status(btc_msg_status_t *out) {
    BTC_LOCK();
    out->started = m.started;
    out->connected = m.connected;
    out->connecting = m.connecting;
    memcpy(out->peer, m.peer, 6);
    out->instance = m.connected ? m.inst[m.cur].id : BTC_UNKNOWN;
    out->features = m.connected ? m.inst[m.cur].features : 0;
    out->types = m.connected ? m.inst[m.cur].types : 0;
    out->instances = m.n_inst;
    out->busy = m.busy;
    out->result = m.result;
    out->length = m.len;
    out->truncated = m.truncated;
    out->listing_size = m.listing_size;
    out->new_message = m.new_message;
    strcpy(out->mse_time, m.mse_time);
    strcpy(out->pushed, m.pushed);
    out->notifying = m.notifying;
    out->notify_connected = m.mns_conn != 0;
    out->events = m.ev_count;
    out->events_dropped = m.ev_dropped;
    BTC_UNLOCK();
}

uint32_t btc_msg_changes(void) {
    BTC_LOCK();
    uint32_t c = m.changes;
    m.changes = 0;
    BTC_UNLOCK();
    return c;
}

#else   // no message client in this build

int btc_msg_start(size_t max_bytes) {
    (void)max_bytes;
    return BTC_ERR_UNSUPPORTED;
}
void btc_msg_stop(void) {
}
bool btc_msg_started(void) {
    return false;
}
int btc_msg_connect(const uint8_t *addr, int instance) {
    (void)addr; (void)instance;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_folders(const char *folder, uint16_t offset, uint16_t count) {
    (void)folder; (void)offset; (void)count;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_list(const char *folder, const char *name, uint16_t offset, uint16_t count,
    uint8_t read_filter, uint8_t type_filter, uint32_t parameter_mask, uint8_t subject_length) {
    (void)folder; (void)name; (void)offset; (void)count; (void)read_filter; (void)type_filter;
    (void)parameter_mask; (void)subject_length;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_get(const char *handle, bool attachments, bool utf8) {
    (void)handle; (void)attachments; (void)utf8;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_set_status(const char *handle, uint8_t indicator, bool value) {
    (void)handle; (void)indicator; (void)value;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_push(const char *folder, const char *name, const uint8_t *bmsg, size_t len, bool utf8, bool retry) {
    (void)folder; (void)name; (void)bmsg; (void)len; (void)utf8; (void)retry;
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_update_inbox(void) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_msg_notify(bool on) {
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_msg_data(uint8_t *dst, size_t size) {
    (void)dst; (void)size;
    return 0;
}
void btc_msg_release(void) {
}
size_t btc_msg_event(uint8_t *dst, size_t size) {
    (void)dst; (void)size;
    return 0;
}
void btc_msg_status(btc_msg_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->instance = out->listing_size = out->new_message = BTC_UNKNOWN;
}
uint32_t btc_msg_changes(void) {
    return 0;
}

#endif
