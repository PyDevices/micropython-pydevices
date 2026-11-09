// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Object Push on Bluedroid's OBEX (OPP client and server): sending a file or
// contact card, pulling a device's card, and taking what a device sends. See
// include/btclassic.h; btstack/opp.c is the same thing on BTstack.
//
// ESP-IDF has no OPP API, but it has the GOEP client its phone book client
// runs on and an OBEX server, and both work only on Bluedroid's own task. So
// this registers a handler there under the task's unused object push slot,
// as map.c does for messages: calls from MicroPython post a message, and
// every GOEP, OBEX or SDP callback posts one too.
//
// A send or a pull is one session: find the device's Object Push in SDP,
// open the link, connect, one PUT or GET, disconnect. The server offers
// Object Push in SDP over RFCOMM and keeps one object at a time.
//
// These are Bluedroid internals, so opp.cmake turns this on only for the
// ESP-IDF versions it was written against.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"

#include "btc_bluedroid.h"

#if BTC_OPP

#include "esp_sdp_api.h"

#include "common/bt_target.h"
#include "osi/allocator.h"
#include "bta/bta_sys.h"
#include "stack/btm_api.h"
#include "stack/goepc_api.h"
#include "stack/obex_api.h"

static const char *TAG = "btclassic";

#define OPP_MTU         4096
#define OPP_VCARD_TYPE  "text/x-vcard"
#define OPP_SERVICE     "OBEX Object Push"

// What runs on Bluedroid's task.
enum {
    EV_API_BEGIN = BTA_ID_OPC << 8,
    EV_API_LISTEN,
    EV_API_UNLISTEN,
    EV_API_STOP,
    EV_SDP_DONE,
    EV_GOEP_OPENED,
    EV_GOEP_CLOSED,
    EV_GOEP_RESPONSE,
    EV_OBEX_SERVER,
};

typedef enum {
    C_IDLE = 0,
    C_SEARCH,           // looking for the device's Object Push in SDP
    C_OPEN,             // the link is opening
    C_CONNECT,
    C_REQUEST,          // the PUT or GET
    C_DISCONNECT,
} cstate_t;

typedef struct {
    BT_HDR hdr;
    uint16_t handle;
    uint8_t event;
    union {
        tGOEPC_MSG goep;
        tOBEX_MSG obex;
    };
} cb_msg_t;

static struct {
    bool started;
    uint32_t changes;

    // The client: one send or pull.
    volatile cstate_t cs;
    bool pulling;
    btc_addr_t peer;
    int result;
    int psm, scn;               // where SDP found the device's Object Push
    uint16_t goep;
    uint8_t cid[4];
    bool has_cid;
    uint16_t max_tx, max_rx;
    uint8_t *out;               // the copy being sent
    uint32_t out_len, sent;
    char out_name[BTC_OPP_NAME_MAX];
    char out_type[BTC_OPP_TYPE_MAX];

    // The server: one sender at a time.
    bool listening;
    uint8_t scn_local;
    uint16_t obex_server;
    uint16_t sconn;
    uint16_t smtu;
    uint32_t record;
    bool record_pending;
    bool refusing;              // the current PUT is turned away
    bool in_put;
    uint32_t received, refused;

    // The buffer: a pulled or received object.
    bool held;
    bool truncated;
    uint8_t *buf;
    size_t len, cap, max;
    char name[BTC_OPP_NAME_MAX];
    char type[BTC_OPP_TYPE_MAX];
} o;

static void mark(uint32_t bits) {
    BTC_LOCK();
    o.changes |= bits;
    BTC_UNLOCK();
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

// ---- the buffer ----

static void gather(const uint8_t *data, size_t n) {
    if (n == 0 || data == NULL) {
        return;
    }
    size_t need = o.len + n;
    if (need > o.max) {
        o.truncated = true;
        need = o.max;
        n = need - o.len;
        if (n == 0) {
            return;
        }
    }
    if (need > o.cap) {
        size_t cap = o.cap ? o.cap : 2048;
        while (cap < need) {
            cap *= 2;
        }
        if (cap > o.max) {
            cap = o.max;
        }
        uint8_t *nb = heap_caps_realloc(o.buf, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (nb == NULL) {
            nb = heap_caps_realloc(o.buf, cap, MALLOC_CAP_8BIT);
        }
        if (nb == NULL) {
            o.truncated = true;
            return;
        }
        BTC_LOCK();
        o.buf = nb;
        o.cap = cap;
        BTC_UNLOCK();
    }
    memcpy(o.buf + o.len, data, n);
    BTC_LOCK();
    o.len += n;
    BTC_UNLOCK();
}

static void empty(void) {
    BTC_LOCK();
    uint8_t *b = o.buf;
    o.buf = NULL;
    o.len = o.cap = 0;
    o.truncated = false;
    o.held = false;
    o.name[0] = o.type[0] = 0;
    BTC_UNLOCK();
    heap_caps_free(b);
}

// UTF-8 to UTF-16BE with a terminating zero, for OBEX's Name header.
static uint16_t to_utf16(const char *s, uint8_t *out, uint16_t cap) {
    uint16_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n + 4 <= cap) {
        uint32_t c = *p++;
        if (c >= 0xe0 && p[0] && p[1]) {
            c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f);
            p += 2;
        } else if (c >= 0xc0 && p[0]) {
            c = ((c & 0x1f) << 6) | (p[0] & 0x3f);
            p += 1;
        }
        out[n++] = (uint8_t)(c >> 8);
        out[n++] = (uint8_t)c;
    }
    out[n++] = 0;
    out[n++] = 0;
    return n;
}

// UTF-16BE to UTF-8, cut at a whole character.
static void from_utf16(const uint8_t *in, size_t len, char *out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t c = (uint16_t)(in[i] << 8 | in[i + 1]);
        if (c == 0) {
            break;
        }
        uint8_t b[3];
        int k;
        if (c < 0x80) {
            b[0] = (uint8_t)c;
            k = 1;
        } else if (c < 0x800) {
            b[0] = 0xc0 | (c >> 6);
            b[1] = 0x80 | (c & 0x3f);
            k = 2;
        } else {
            b[0] = 0xe0 | (c >> 12);
            b[1] = 0x80 | ((c >> 6) & 0x3f);
            b[2] = 0x80 | (c & 0x3f);
            k = 3;
        }
        if (n + k >= cap) {
            break;
        }
        memcpy(out + n, b, k);
        n += k;
    }
    out[n] = 0;
}

// ---- the client ----

static void client_done(int result) {
    uint8_t *out = o.out;
    BTC_LOCK();
    o.result = result;
    o.out = NULL;
    o.cs = C_IDLE;
    BTC_UNLOCK();
    if (out) {
        heap_caps_free(out);
    }
    o.goep = 0;
    mark(BTC_CHANGED_PUSH | BTC_CHANGED_CONNECTION);
}

// The session ends with OBEX's disconnect, then the link's.
static void client_finish(int result) {
    o.result = result;
    o.cs = C_DISCONNECT;
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_DISCONNECT;
    uint16_t ret = GOEPC_PrepareRequest(o.goep, &info, BT_SMALL_BUFFER_SIZE);
    if (o.has_cid) {
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_CONNECTION_ID, o.cid, 4);
    }
    ret |= GOEPC_SendRequest(o.goep);
    if (ret != GOEP_SUCCESS) {
        GOEPC_Close(o.goep);
        client_done(result);
    }
}

static void link_failed(void) {
    ESP_LOGW(TAG, "object push: the request couldn't be sent");
    GOEPC_Close(o.goep);
    client_done(BTC_ERR_FAIL);
}

static void send_get(void) {
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_GET_FINAL;
    uint16_t ret = GOEPC_PrepareRequest(o.goep, &info, o.max_tx);
    if (o.has_cid) {
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_CONNECTION_ID, o.cid, 4);
    }
    if (o.cs == C_CONNECT) {
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_TYPE, (const uint8_t *)OPP_VCARD_TYPE, sizeof(OPP_VCARD_TYPE));
    }
    o.cs = C_REQUEST;
    ret |= GOEPC_SendRequest(o.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
    }
}

// The next PUT packet: the headers with the first, then as much of the body
// as fits, End-of-Body with the last.
static void send_put(void) {
    bool first = o.cs == C_CONNECT;
    size_t room = o.max_tx - 3 - 3 - (o.has_cid ? 5 : 0);   // the packet's head, the body header's
    tOBEX_PARSE_INFO info = {0};
    uint8_t name16[2 * BTC_OPP_NAME_MAX + 2];
    uint16_t name16_len = 0;
    if (first) {
        name16_len = to_utf16(o.out_name, name16, sizeof(name16));
        room -= 3 + name16_len + 5;                 // Name, Length
        if (o.out_type[0]) {
            room -= 3 + strlen(o.out_type) + 1;     // Type
        }
    }
    uint32_t left = o.out_len - o.sent;
    uint32_t n = left < room ? left : (uint32_t)room;
    bool last = n == left;
    info.opcode = last ? OBEX_OPCODE_PUT_FINAL : OBEX_OPCODE_PUT;
    uint16_t ret = GOEPC_PrepareRequest(o.goep, &info, o.max_tx);
    if (o.has_cid) {
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_CONNECTION_ID, o.cid, 4);
    }
    if (first) {
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_NAME, name16, name16_len);
        if (o.out_type[0]) {
            ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_TYPE, (const uint8_t *)o.out_type, strlen(o.out_type) + 1);
        }
        uint8_t length[4] = { o.out_len >> 24, o.out_len >> 16, o.out_len >> 8, o.out_len };
        ret |= GOEPC_RequestAddHeader(o.goep, OBEX_HEADER_ID_LENGTH, length, 4);
    }
    ret |= GOEPC_RequestAddHeader(o.goep, last ? OBEX_HEADER_ID_END_OF_BODY : OBEX_HEADER_ID_BODY,
        n ? o.out + o.sent : NULL, n);
    o.cs = C_REQUEST;
    ret |= GOEPC_SendRequest(o.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
        return;
    }
    BTC_LOCK();
    o.sent += n;
    BTC_UNLOCK();
}

static void on_response(cb_msg_t *c) {
    BT_HDR *pkt = c->goep.response.pkt;
    uint8_t opcode = c->goep.response.opcode;
    tOBEX_PARSE_INFO info;
    OBEX_ParseResponse(pkt, opcode, &info);
    uint8_t rc = info.response_code & 0x7f;
    uint8_t *h;

    if (opcode == OBEX_OPCODE_CONNECT) {
        if (rc != OBEX_RESPONSE_CODE_OK) {
            ESP_LOGW(TAG, "object push: the device refused the session: 0x%02x", rc);
            GOEPC_Close(o.goep);
            client_done(rc | 0x80);
        } else {
            if (info.max_packet_length >= 255 && info.max_packet_length < o.max_tx) {
                o.max_tx = info.max_packet_length;
            }
            while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                if (*h == OBEX_HEADER_ID_CONNECTION_ID) {
                    memcpy(o.cid, h + 1, 4);
                    o.has_cid = true;
                }
            }
            if (o.pulling) {
                send_get();
            } else {
                send_put();
            }
        }
    } else if (opcode == OBEX_OPCODE_DISCONNECT) {
        GOEPC_Close(o.goep);
        client_done(o.result);
    } else if (opcode == OBEX_OPCODE_GET || opcode == OBEX_OPCODE_GET_FINAL) {
        if (rc == OBEX_RESPONSE_CODE_OK || rc == OBEX_RESPONSE_CODE_CONTINUE) {
            while ((h = OBEX_GetNextHeader(pkt, &info)) != NULL) {
                if (*h == OBEX_HEADER_ID_BODY || *h == OBEX_HEADER_ID_END_OF_BODY) {
                    gather(h + 3, hdr_value_len(pkt, h));
                }
            }
        }
        if (rc == OBEX_RESPONSE_CODE_CONTINUE) {
            if (!c->goep.response.srm_en || c->goep.response.srm_wait) {
                send_get();
            }
        } else if (rc == OBEX_RESPONSE_CODE_OK) {
            BTC_LOCK();
            o.held = true;
            strcpy(o.type, OPP_VCARD_TYPE);
            BTC_UNLOCK();
            client_finish(BTC_OK);
        } else {
            client_finish(rc | 0x80);
        }
    } else if (opcode == OBEX_OPCODE_PUT || opcode == OBEX_OPCODE_PUT_FINAL) {
        if (rc == OBEX_RESPONSE_CODE_CONTINUE && o.sent < o.out_len) {
            send_put();
        } else if (rc == OBEX_RESPONSE_CODE_OK || rc == OBEX_RESPONSE_CODE_CONTINUE) {
            client_finish(BTC_OK);
        } else {
            client_finish(rc | 0x80);
        }
    }
    osi_free(pkt);
}

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
    if (c->goep.opened.peer_mtu && c->goep.opened.peer_mtu < o.max_tx) {
        o.max_tx = c->goep.opened.peer_mtu;
    }
    if (c->goep.opened.our_mtu && c->goep.opened.our_mtu < o.max_rx) {
        o.max_rx = c->goep.opened.our_mtu;
    }
    tOBEX_PARSE_INFO info = {0};
    info.opcode = OBEX_OPCODE_CONNECT;
    info.obex_version_number = OBEX_VERSION_NUMBER;
    info.max_packet_length = o.max_rx;
    o.cs = C_CONNECT;
    uint16_t ret = GOEPC_PrepareRequest(o.goep, &info, BT_SMALL_BUFFER_SIZE);
    ret |= GOEPC_SendRequest(o.goep);
    if (ret != GOEP_SUCCESS) {
        link_failed();
    }
}

// A close before the session ended means the device dropped it.
static void on_closed(uint16_t handle) {
    if (o.cs == C_IDLE || handle != o.goep) {
        return;
    }
    int result = o.result;
    if (o.cs == C_OPEN) {
        ESP_LOGW(TAG, "object push: the link didn't open");
        result = BTC_ERR_FAIL;
    } else if (o.cs != C_DISCONNECT && result == BTC_OK) {
        result = BTC_ERR_STATE;
    }
    client_done(result);
}

// SDP found the device's Object Push, or didn't: open the link, over L2CAP
// where the device offers it.
static void on_sdp_done(void) {
    if (o.cs != C_SEARCH) {
        return;
    }
    if (o.psm <= 0 && o.scn <= 0) {
        ESP_LOGW(TAG, "object push: the device offers no Object Push");
        client_done(BTC_ERR_FAIL);
        return;
    }
    tOBEX_SVR_INFO svr = {0};
    if (o.psm > 0) {
        svr.tl = OBEX_OVER_L2CAP;
        svr.l2cap.psm = o.psm;
        svr.l2cap.sec_mask = BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT;
        memcpy(svr.l2cap.addr, o.peer, 6);
    } else {
        svr.tl = OBEX_OVER_RFCOMM;
        svr.rfcomm.scn = o.scn;
        svr.rfcomm.sec_mask = BTM_SEC_OUT_AUTHENTICATE | BTM_SEC_OUT_ENCRYPT;
        memcpy(svr.rfcomm.addr, o.peer, 6);
    }
    o.max_tx = o.max_rx = OPP_MTU;
    o.has_cid = false;
    o.cs = C_OPEN;
    if (GOEPC_Open(&svr, goep_cb, &o.goep) != GOEP_SUCCESS) {
        o.goep = 0;
        client_done(BTC_ERR_FAIL);
    }
}

static void on_begin(void) {
    o.psm = o.scn = -1;
    esp_bt_uuid_t uuid = {.len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_SDP_UUID_OPP};
    if (esp_sdp_search_record(o.peer, uuid) != ESP_OK) {
        client_done(BTC_ERR_FAIL);
    }
}

// ---- the server ----

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

static void respond(uint16_t conn, uint8_t opcode, uint8_t code) {
    tOBEX_PARSE_INFO info = {0};
    info.opcode = opcode;
    info.response_code = code | OBEX_FINAL_BIT_MASK;
    if (opcode == OBEX_OPCODE_CONNECT) {
        // Bluedroid's RFCOMM hands each frame up whole and alone, so a
        // packet must fit one: the link's MTU caps it.
        info.obex_version_number = OBEX_VERSION_NUMBER;
        info.max_packet_length = o.smtu >= 255 && o.smtu < OPP_MTU ? o.smtu : OPP_MTU;
    }
    BT_HDR *pkt = NULL;
    if (OBEX_BuildResponse(&info, 256, &pkt) != OBEX_SUCCESS) {
        return;
    }
    OBEX_SendPacket(conn, pkt);
}

// The sender: the paired device whose link is up. OBEX doesn't say.
static void find_sender(void) {
    btc_addr_t paired[8];
    int n = btc_radio_paired(paired, 8);
    for (int i = 0; i < n && i < 8; i++) {
        if (BTM_IsAclConnectionUp(paired[i], BT_TRANSPORT_BR_EDR)) {
            BTC_LOCK();
            memcpy(o.peer, paired[i], 6);
            BTC_UNLOCK();
            return;
        }
    }
}

static void on_put(cb_msg_t *c, BT_HDR *pkt, tOBEX_PARSE_INFO *info, bool final) {
    if (!o.in_put) {
        // A new object: its first packet decides if it's taken.
        o.in_put = true;
        o.refusing = o.held || !o.started || (o.cs != C_IDLE && o.pulling);
        if (!o.refusing) {
            empty();
            mark(BTC_CHANGED_PUSH);
        }
    }
    if (!o.refusing) {
        uint8_t *h;
        while ((h = OBEX_GetNextHeader(pkt, info)) != NULL) {
            size_t n = hdr_value_len(pkt, h);
            if (*h == OBEX_HEADER_ID_NAME) {
                char name[BTC_OPP_NAME_MAX];
                from_utf16(h + 3, n, name, sizeof(name));
                BTC_LOCK();
                strcpy(o.name, name);
                BTC_UNLOCK();
            } else if (*h == OBEX_HEADER_ID_TYPE) {
                size_t k = n < sizeof(o.type) - 1 ? n : sizeof(o.type) - 1;
                BTC_LOCK();
                memcpy(o.type, h + 3, k);
                o.type[k] = 0;
                BTC_UNLOCK();
            } else if (*h == OBEX_HEADER_ID_BODY || *h == OBEX_HEADER_ID_END_OF_BODY) {
                gather(h + 3, n);
            }
        }
    }
    if (!final) {
        respond(c->handle, info->opcode, o.refusing ? OBEX_RESPONSE_CODE_SERVICE_UNAVAILABLE : OBEX_RESPONSE_CODE_CONTINUE);
        return;
    }
    if (o.refusing) {
        BTC_LOCK();
        o.refused++;
        BTC_UNLOCK();
        respond(c->handle, info->opcode, OBEX_RESPONSE_CODE_SERVICE_UNAVAILABLE);
    } else {
        BTC_LOCK();
        o.held = true;
        o.received++;
        BTC_UNLOCK();
        ESP_LOGI(TAG, "object push: took %u bytes", (unsigned)o.len);
        mark(BTC_CHANGED_PUSH);
        respond(c->handle, info->opcode, OBEX_RESPONSE_CODE_OK);
    }
    o.in_put = false;
}

static void on_server(cb_msg_t *c) {
    switch (c->event) {
        case OBEX_CONN_INCOME_EVT:
            if (o.sconn || !o.listening) {
                OBEX_RemoveConn(c->handle);     // one sender at a time
                break;
            }
            o.sconn = c->handle;
            o.smtu = c->obex.conn_income.our_mtu;
            o.in_put = o.refusing = false;
            find_sender();
            mark(BTC_CHANGED_CONNECTION);
            break;
        case OBEX_DISCONNECT_EVT:
            if (c->handle != o.sconn) {
                break;
            }
            if (o.in_put && !o.refusing) {
                empty();        // the sender went before the last packet
            }
            o.sconn = 0;
            o.in_put = o.refusing = false;
            mark(BTC_CHANGED_CONNECTION);
            break;
        case OBEX_DATA_EVT: {
            BT_HDR *pkt = c->obex.data.pkt;
            if (c->handle != o.sconn) {
                osi_free(pkt);
                break;
            }
            tOBEX_PARSE_INFO info = {0};
            OBEX_ParseRequest(pkt, &info);
            uint8_t op = info.opcode;
            if (op == OBEX_OPCODE_CONNECT || op == OBEX_OPCODE_DISCONNECT) {
                respond(c->handle, op, OBEX_RESPONSE_CODE_OK);
            } else if (op == OBEX_OPCODE_PUT || op == OBEX_OPCODE_PUT_FINAL) {
                on_put(c, pkt, &info, op == OBEX_OPCODE_PUT_FINAL);
            } else if (op == OBEX_OPCODE_ABORT) {
                if (o.in_put && !o.refusing) {
                    empty();
                }
                o.in_put = false;
                respond(c->handle, op, OBEX_RESPONSE_CODE_OK);
            } else {
                respond(c->handle, op, OBEX_RESPONSE_CODE_NOT_IMPLEMENTED);
            }
            osi_free(pkt);
            break;
        }
        default:
            break;
    }
}

// Offer Object Push: an OBEX server on an RFCOMM channel, and its SDP record.
static void on_listen(void) {
    if (o.obex_server) {
        return;
    }
    o.scn_local = BTM_AllocateSCN();
    if (o.scn_local == 0) {
        ESP_LOGW(TAG, "object push: no RFCOMM channel");
        return;
    }
    tOBEX_SVR_INFO svr = {0};
    svr.tl = OBEX_OVER_RFCOMM;
    svr.rfcomm.scn = o.scn_local;
    svr.rfcomm.sec_mask = BTM_SEC_IN_AUTHENTICATE | BTM_SEC_IN_ENCRYPT;
    if (OBEX_RegisterServer(&svr, obex_server_cb, &o.obex_server) != OBEX_SUCCESS) {
        ESP_LOGW(TAG, "object push: the server didn't start");
        BTM_FreeSCN(o.scn_local);
        o.scn_local = 0;
        o.obex_server = 0;
        return;
    }
    esp_bluetooth_sdp_record_t rec = {0};
    rec.ops.hdr.type = ESP_SDP_TYPE_OPP_SERVER;
    rec.ops.hdr.service_name = OPP_SERVICE;
    rec.ops.hdr.service_name_length = sizeof(OPP_SERVICE);   // with its NUL, which esp_sdp checks
    rec.ops.hdr.rfcomm_channel_number = o.scn_local;
    rec.ops.hdr.l2cap_psm = -1;
    rec.ops.hdr.profile_version = 0x0102;
    rec.ops.supported_formats_list_len = 1;
    rec.ops.supported_formats_list[0] = 0xff;                   // any format
    o.record_pending = true;
    if (esp_sdp_create_record(&rec) != ESP_OK) {
        o.record_pending = false;
        ESP_LOGW(TAG, "object push: the SDP record wasn't made");
    }
}

static void on_unlisten(void) {
    if (o.sconn) {
        OBEX_RemoveConn(o.sconn);
        if (o.in_put && !o.refusing) {
            empty();
        }
        o.sconn = 0;
        o.in_put = o.refusing = false;
    }
    if (o.obex_server) {
        OBEX_DeregisterServer(o.obex_server);
        o.obex_server = 0;
    }
    if (o.scn_local) {
        BTM_FreeSCN(o.scn_local);
        o.scn_local = 0;
    }
}

// ---- SDP: the device's Object Push, and the record for ours ----

static void sdp_cb(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param) {
    if (event == ESP_SDP_SEARCH_COMP_EVT && o.cs == C_SEARCH
        && param->search.sdp_uuid.uuid.uuid16 == ESP_SDP_UUID_OPP
        && memcmp(param->search.remote_addr, o.peer, 6) == 0) {
        if (param->search.status == ESP_SDP_SUCCESS) {
            for (int i = 0; i < param->search.record_count; i++) {
                esp_bluetooth_sdp_record_t *rec = &param->search.records[i];
                if (rec->hdr.type == ESP_SDP_TYPE_OPP_SERVER) {
                    o.psm = rec->hdr.l2cap_psm;
                    o.scn = rec->hdr.rfcomm_channel_number;
                    break;
                }
            }
        }
        BT_HDR *c = osi_malloc(sizeof(*c));
        if (c) {
            c->event = EV_SDP_DONE;
            bta_sys_sendmsg(c);
        }
    } else if (event == ESP_SDP_CREATE_RECORD_COMP_EVT && o.record_pending) {
        // SDP is shared: a completion is ours only while ours is pending.
        BTC_LOCK();
        o.record_pending = false;
        if (param->create_record.status == ESP_SDP_SUCCESS) {
            o.record = param->create_record.record_handle;
        }
        BTC_UNLOCK();
        if (!o.record) {
            ESP_LOGW(TAG, "object push: the SDP record wasn't made (status %d)", param->create_record.status);
        }
    }
}

// ---- Bluedroid's task ----

static BOOLEAN task_handler(BT_HDR *msg) {
    switch (msg->event) {
        case EV_API_BEGIN:
            on_begin();
            break;
        case EV_API_LISTEN:
            on_listen();
            break;
        case EV_API_UNLISTEN:
            on_unlisten();
            break;
        case EV_API_STOP:
            on_unlisten();
            if (o.goep) {
                GOEPC_Close(o.goep);
            }
            if (o.cs != C_IDLE) {
                client_done(BTC_ERR_STATE);
            }
            break;
        case EV_SDP_DONE:
            on_sdp_done();
            break;
        case EV_GOEP_OPENED:
            if (o.cs == C_OPEN && ((cb_msg_t *)msg)->handle == o.goep) {
                on_opened((cb_msg_t *)msg);
            }
            break;
        case EV_GOEP_CLOSED:
            on_closed(((cb_msg_t *)msg)->handle);
            break;
        case EV_GOEP_RESPONSE: {
            cb_msg_t *c = (cb_msg_t *)msg;
            if (o.cs != C_IDLE && c->handle == o.goep) {
                on_response(c);
            } else {
                osi_free(c->goep.response.pkt);
            }
            break;
        }
        case EV_OBEX_SERVER:
            on_server((cb_msg_t *)msg);
            break;
        default:
            break;
    }
    return TRUE;
}

static const tBTA_SYS_REG task_reg = { task_handler, NULL };

static bool post(uint16_t event) {
    BT_HDR *c = osi_malloc(sizeof(*c));
    if (c == NULL) {
        return false;
    }
    c->event = event;
    bta_sys_sendmsg(c);
    return true;
}

// ---- the API ----

int btc_opp_start(size_t max_bytes) {
    if (o.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (max_bytes < 1024) {
        return BTC_ERR_ARG;
    }
    memset(&o, 0, sizeof(o));
    o.max = max_bytes;
    esp_err_t err = btc_sdp_open(sdp_cb);    // shared with HIDDevice and Messages
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "object push: SDP didn't start: %s", esp_err_to_name(err));
        return btc_err(err);
    }
    bta_sys_register(BTA_ID_OPC, &task_reg);
    o.started = true;
    return BTC_OK;
}

void btc_opp_stop(void) {
    if (!o.started) {
        return;
    }
    if (o.listening) {
        btc_opp_listen(false);
    }
    if (o.cs != C_IDLE && post(EV_API_STOP)) {
        for (int i = 0; i < 100 && o.cs != C_IDLE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    o.started = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    btc_sdp_close(sdp_cb);
    empty();
}

bool btc_opp_started(void) {
    return o.started;
}

static int client_begin(const uint8_t *addr) {
    if (!o.started) {
        return BTC_ERR_STATE;
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
    BTC_LOCK();
    if (o.cs != C_IDLE) {
        BTC_UNLOCK();
        return BTC_ERR_STATE;
    }
    o.cs = C_SEARCH;
    memcpy(o.peer, a, 6);
    o.result = BTC_OK;
    o.sent = 0;
    BTC_UNLOCK();
    return BTC_OK;
}

int btc_opp_send(const uint8_t *addr, const char *name, const char *type, const uint8_t *data, size_t len) {
    if (name == NULL || name[0] == 0 || strlen(name) >= BTC_OPP_NAME_MAX
        || (type && strlen(type) >= BTC_OPP_TYPE_MAX) || len > 0xffffffffu) {
        return BTC_ERR_ARG;
    }
    if (!o.started || o.cs != C_IDLE) {
        return BTC_ERR_STATE;
    }
    uint8_t *copy = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        copy = heap_caps_malloc(len ? len : 1, MALLOC_CAP_8BIT);
    }
    if (copy == NULL) {
        return BTC_ERR_NOMEM;
    }
    memcpy(copy, data, len);
    int err = client_begin(addr);
    if (err) {
        heap_caps_free(copy);
        return err;
    }
    o.out = copy;
    o.out_len = (uint32_t)len;
    o.pulling = false;
    strcpy(o.out_name, name);
    strcpy(o.out_type, type ? type : "");
    if (!post(EV_API_BEGIN)) {
        client_done(BTC_ERR_NOMEM);
        return BTC_ERR_NOMEM;
    }
    return BTC_OK;
}

int btc_opp_pull(const uint8_t *addr) {
    if (o.started && o.held) {
        return BTC_ERR_STATE;       // the buffer holds an object: take it first
    }
    if (o.started && o.in_put) {
        return BTC_ERR_STATE;       // a device is sending into the buffer
    }
    int err = client_begin(addr);
    if (err) {
        return err;
    }
    empty();
    o.pulling = true;
    if (!post(EV_API_BEGIN)) {
        client_done(BTC_ERR_NOMEM);
        return BTC_ERR_NOMEM;
    }
    return BTC_OK;
}

int btc_opp_listen(bool on) {
    if (!o.started) {
        return BTC_ERR_STATE;
    }
    if (on == o.listening) {
        return BTC_OK;
    }
    if (on) {
        if (!post(EV_API_LISTEN)) {
            return BTC_ERR_NOMEM;
        }
        for (int i = 0; i < 50 && (o.record_pending || !o.obex_server); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        // Without its record no device finds the server.
        if (!o.obex_server || !o.record) {
            if (o.record) {
                esp_sdp_remove_record(o.record);
                o.record = 0;
            }
            post(EV_API_UNLISTEN);
            return BTC_ERR_FAIL;
        }
        o.listening = true;
    } else {
        o.listening = false;
        if (o.record) {
            esp_sdp_remove_record(o.record);
            o.record = 0;
        }
        if (post(EV_API_UNLISTEN)) {
            for (int i = 0; i < 50 && (o.obex_server || o.sconn); i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    btc_radio_profile(BTC_PROFILE_PUSH, on, 0);
    return BTC_OK;
}

size_t btc_opp_data(uint8_t *dst, size_t size) {
    if (!o.held || o.buf == NULL) {
        return 0;
    }
    size_t n = o.len < size ? o.len : size;
    memcpy(dst, o.buf, n);
    return n;
}

const char *btc_opp_name(void) {
    return o.name;
}

const char *btc_opp_type(void) {
    return o.type;
}

// Only a held object: an object coming in fills the buffer on Bluedroid's task.
void btc_opp_take(void) {
    if (o.held) {
        empty();
    }
}

void btc_opp_status(btc_opp_status_t *out) {
    memset(out, 0, sizeof(*out));
    BTC_LOCK();
    out->started = o.started;
    out->busy = o.cs != C_IDLE;
    out->listening = o.listening;
    out->receiving = o.sconn && o.in_put && !o.refusing;
    out->held = o.held;
    memcpy(out->peer, o.peer, 6);
    out->result = o.result;
    out->sent = o.sent;
    out->length = o.held ? (uint32_t)o.len : 0;
    out->truncated = o.truncated;
    out->received = o.received;
    out->refused = o.refused;
    BTC_UNLOCK();
}

uint32_t btc_opp_changes(void) {
    BTC_LOCK();
    uint32_t c = o.changes;
    o.changes = 0;
    BTC_UNLOCK();
    return c;
}

#else   // no object push in this build

int btc_opp_start(size_t max_bytes) {
    (void)max_bytes;
    return BTC_ERR_UNSUPPORTED;
}
void btc_opp_stop(void) {
}
bool btc_opp_started(void) {
    return false;
}
int btc_opp_send(const uint8_t *addr, const char *name, const char *type, const uint8_t *data, size_t len) {
    (void)addr; (void)name; (void)type; (void)data; (void)len;
    return BTC_ERR_UNSUPPORTED;
}
int btc_opp_pull(const uint8_t *addr) {
    (void)addr;
    return BTC_ERR_UNSUPPORTED;
}
int btc_opp_listen(bool on) {
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}
size_t btc_opp_data(uint8_t *dst, size_t size) {
    (void)dst; (void)size;
    return 0;
}
const char *btc_opp_name(void) {
    return "";
}
const char *btc_opp_type(void) {
    return "";
}
void btc_opp_take(void) {
}
void btc_opp_status(btc_opp_status_t *out) {
    memset(out, 0, sizeof(*out));
}
uint32_t btc_opp_changes(void) {
    return 0;
}

#endif
