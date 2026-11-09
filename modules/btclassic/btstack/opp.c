// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Object Push on BTstack's GOEP: sending a file or contact card, pulling a
// device's card, and taking what a device sends. See btclassic.h.
//
// BTstack has no OPP module, so this speaks OBEX itself. A send or a pull is
// one session: connect, one PUT or GET, disconnect. The server side offers
// Object Push in SDP over RFCOMM, and keeps one object at a time.

#include <string.h>

#include "btstack.h"
#include "classic/goep_client.h"
#include "classic/goep_server.h"
#include "classic/obex.h"
#include "classic/obex_parser.h"

#include "btc_btstack.h"

#define OPP_RFCOMM_CHANNEL 3
#define OPP_ERTM_SIZE 4400
#define OPP_VCARD_TYPE "text/x-vcard"
#ifndef OBEX_RESP_SERVICE_UNAVAILABLE
#define OBEX_RESP_SERVICE_UNAVAILABLE 0xD3
#endif

// goep_server.h names it geop_server_deinit; goep_server.c defines this.
void goep_server_deinit(void);

typedef enum {
    C_IDLE = 0,
    C_W4_GOEP,          // the bearer is opening
    C_W2_CONNECT,
    C_W4_CONNECT,
    C_W2_REQUEST,       // the next PUT or GET packet
    C_W4_REQUEST,
    C_W2_DISCONNECT,
    C_W4_DISCONNECT,
    C_W4_CLOSE,         // the bearer is closing
} cstate_t;

static struct {
    bool started;
    bool server_up;         // goep_server_init done while the radio is on
    bool listening;
    uint32_t changes;
    uint32_t sdp_handle;

    // The client: one send or pull.
    cstate_t cs;
    bool pulling;
    bool first;             // the first PUT packet carries the headers
    goep_client_t client;
    uint16_t ccid;
    btc_addr_t peer;
    obex_parser_t cparser;
    int result;
    uint8_t *out;           // the copy being sent
    uint32_t out_len, sent;
    char out_name[BTC_OPP_NAME_MAX];
    char out_type[BTC_OPP_TYPE_MAX];
    uint8_t *ertm_raw, *ertm;
    l2cap_ertm_config_t ertm_config;

    // The server: one sender at a time.
    uint16_t scid;
    bool sconnected;
    obex_parser_t sparser;
    bool sparsing;
    uint8_t sresponse;      // waiting to go, or 0
    bool sconnect_reply;
    bool refusing;          // the current PUT is turned away
    bool in_put;
    uint8_t name16[2 * BTC_OPP_NAME_MAX];
    uint16_t name16_len;
    uint32_t received, refused;

    // The buffer: a pulled or received object.
    bool held;
    bool truncated;
    uint8_t *buf;
    size_t len, cap, max;
    char name[BTC_OPP_NAME_MAX];
    char type[BTC_OPP_TYPE_MAX];
} o;

static uint8_t sdp_opp[150];

static int obex_result(uint8_t code) {
    code &= 0x7f;
    return code == (OBEX_RESP_SUCCESS & 0x7f) ? BTC_OK : code | 0x80;
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
        uint8_t *nb = btc_btstack_alloc(cap);
        if (nb == NULL) {
            o.truncated = true;
            return;
        }
        if (o.buf) {
            memcpy(nb, o.buf, o.len);
            btc_btstack_free(o.buf);
        }
        o.buf = nb;
        o.cap = cap;
    }
    memcpy(o.buf + o.len, data, n);
    o.len += n;
}

static void empty(void) {
    if (o.buf) {
        btc_btstack_free(o.buf);
    }
    o.buf = NULL;
    o.len = o.cap = 0;
    o.truncated = false;
    o.held = false;
    o.name[0] = o.type[0] = 0;
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
static void from_utf16(const uint8_t *in, uint16_t len, char *out, size_t cap) {
    size_t n = 0;
    for (uint16_t i = 0; i + 1 < len; i += 2) {
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
    o.result = result;
    if (o.out) {
        btc_btstack_free(o.out);
        o.out = NULL;
    }
    o.changes |= BTC_CHANGED_PUSH;
}

static void free_ertm(void) {
    if (o.ertm_raw) {
        btc_btstack_free(o.ertm_raw);
    }
    o.ertm_raw = o.ertm = NULL;
}

static void on_client_header(void *user, uint8_t id, uint16_t total, uint16_t off, const uint8_t *data, uint16_t len) {
    (void)user;
    (void)total;
    (void)off;
    if (o.pulling && (id == OBEX_HEADER_BODY || id == OBEX_HEADER_END_OF_BODY)) {
        gather(data, len);
    }
}

static void expect(uint8_t opcode) {
    obex_parser_init_for_response(&o.cparser, opcode, on_client_header, NULL);
}

static void client_send(void) {
    switch (o.cs) {
        case C_W2_CONNECT:
            goep_client_request_create_connect(o.ccid, OBEX_VERSION, 0, OBEX_MAX_PACKETLEN_DEFAULT);
            o.cs = C_W4_CONNECT;
            expect(OBEX_OPCODE_CONNECT);
            goep_client_execute(o.ccid);
            break;
        case C_W2_REQUEST:
            if (o.pulling) {
                goep_client_request_create_get(o.ccid);
                if (o.first) {
                    goep_client_header_add_type(o.ccid, OPP_VCARD_TYPE);
                    o.first = false;
                }
                o.cs = C_W4_REQUEST;
                expect(OBEX_OPCODE_GET);
                goep_client_execute(o.ccid);
            } else {
                goep_client_request_create_put(o.ccid);
                if (o.first) {
                    uint8_t name16[2 * BTC_OPP_NAME_MAX + 2];
                    uint16_t n = to_utf16(o.out_name, name16, sizeof(name16));
                    goep_client_header_add_variable(o.ccid, OBEX_HEADER_NAME, name16, n);
                    if (o.out_type[0]) {
                        goep_client_header_add_type(o.ccid, o.out_type);
                    }
                    goep_client_header_add_length(o.ccid, o.out_len);
                    o.first = false;
                }
                // As much as fits, in Body headers and End-of-Body last.
                uint16_t room = goep_client_request_get_max_body_size(o.ccid);
                uint32_t n = room > 3 ? room - 3u : 0;
                if (n > o.out_len - o.sent) {
                    n = o.out_len - o.sent;
                }
                bool final = o.sent + n >= o.out_len;
                goep_client_header_add_variable(o.ccid, final ? OBEX_HEADER_END_OF_BODY : OBEX_HEADER_BODY,
                    o.out + o.sent, (uint16_t)n);
                o.sent += n;
                o.cs = C_W4_REQUEST;
                expect(OBEX_OPCODE_PUT);
                goep_client_execute_with_final_bit(o.ccid, final);
            }
            break;
        case C_W2_DISCONNECT:
            goep_client_request_create_disconnect(o.ccid);
            o.cs = C_W4_DISCONNECT;
            expect(OBEX_OPCODE_DISCONNECT);
            goep_client_execute(o.ccid);
            break;
        default:
            break;
    }
}

static void client_next(cstate_t s) {
    o.cs = s;
    goep_client_request_can_send_now(o.ccid);
}

// The session ends with OBEX's disconnect, then the bearer's.
static void client_finish(int result) {
    o.result = result;
    client_next(C_W2_DISCONNECT);
}

static void client_response(void) {
    obex_parser_operation_info_t info;
    obex_parser_get_operation_info(&o.cparser, &info);
    uint8_t code = info.response_code;
    switch (o.cs) {
        case C_W4_CONNECT:
            if (code == OBEX_RESP_SUCCESS) {
                o.first = true;
                client_next(C_W2_REQUEST);
            } else {
                client_finish(obex_result(code));
            }
            break;
        case C_W4_REQUEST:
            if (code == OBEX_RESP_CONTINUE) {
                client_next(C_W2_REQUEST);
            } else {
                if (o.pulling && code == OBEX_RESP_SUCCESS) {
                    o.held = true;
                    strcpy(o.type, OPP_VCARD_TYPE);
                }
                client_finish(obex_result(code));
            }
            break;
        case C_W4_DISCONNECT:
            o.cs = C_W4_CLOSE;
            goep_client_disconnect(o.ccid);
            break;
        default:
            break;
    }
}

static void on_client(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == GOEP_DATA_PACKET) {
        if (obex_parser_process_data(&o.cparser, packet, size) == OBEX_PARSER_OBJECT_STATE_COMPLETE) {
            client_response();
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GOEP_META) {
        return;
    }
    switch (hci_event_goep_meta_get_subevent_code(packet)) {
        case GOEP_SUBEVENT_CONNECTION_OPENED: {
            uint8_t status = goep_subevent_connection_opened_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                btc_log("object push didn't connect: 0x%02x", status);
                o.cs = C_IDLE;
                free_ertm();
                client_done(BTC_ERR_FAIL);
                break;
            }
            o.ccid = goep_subevent_connection_opened_get_goep_cid(packet);
            client_next(C_W2_CONNECT);
            o.changes |= BTC_CHANGED_CONNECTION;
            break;
        }
        case GOEP_SUBEVENT_CAN_SEND_NOW:
            client_send();
            break;
        case GOEP_SUBEVENT_CONNECTION_CLOSED: {
            // A close before the reply means the device dropped the session.
            bool early = o.cs != C_W4_CLOSE && o.cs != C_W4_DISCONNECT;
            o.cs = C_IDLE;
            o.ccid = 0;
            free_ertm();
            client_done(early && o.result == BTC_OK ? BTC_ERR_STATE : o.result);
            o.changes |= BTC_CHANGED_CONNECTION;
            break;
        }
        default:
            break;
    }
}

static int client_begin(const uint8_t *addr) {
    if (!o.started) {
        return BTC_ERR_STATE;
    }
    if (o.cs != C_IDLE) {
        return BTC_ERR_STATE;
    }
    if (addr) {
        memcpy(o.peer, addr, sizeof(o.peer));
    } else if (!btc_radio_last_peer(o.peer)) {
        btc_addr_t paired[1];
        if (btc_radio_paired(paired, 1) < 1) {
            return BTC_ERR_STATE;
        }
        memcpy(o.peer, paired[0], sizeof(o.peer));
    }
    o.ertm_raw = btc_btstack_alloc(OPP_ERTM_SIZE + 16);
    if (o.ertm_raw == NULL) {
        return BTC_ERR_NOMEM;
    }
    o.ertm = (uint8_t *)(((uintptr_t)o.ertm_raw + 15) & ~(uintptr_t)15);
    o.ertm_config = (l2cap_ertm_config_t){ 1, 2, 2000, 12000, 1000, 2, 2, 1 };
    o.result = BTC_OK;
    o.sent = 0;
    o.cs = C_W4_GOEP;
    uint8_t s = goep_client_connect(&o.client, &o.ertm_config, o.ertm, OPP_ERTM_SIZE, &on_client, o.peer,
        BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH, 0, &o.ccid);
    if (s != ERROR_CODE_SUCCESS) {
        o.cs = C_IDLE;
        free_ertm();
        return BTC_ERR_FAIL;
    }
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
    uint8_t *copy = btc_btstack_alloc(len ? len : 1);
    if (copy == NULL) {
        return BTC_ERR_NOMEM;
    }
    memcpy(copy, data, len);
    int err = client_begin(addr);
    if (err) {
        btc_btstack_free(copy);
        return err;
    }
    o.out = copy;
    o.out_len = (uint32_t)len;
    o.pulling = false;
    strcpy(o.out_name, name);
    strcpy(o.out_type, type ? type : "");
    return BTC_OK;
}

int btc_opp_pull(const uint8_t *addr) {
    if (o.started && o.held) {
        return BTC_ERR_STATE;       // the buffer holds an object: take it first
    }
    int err = client_begin(addr);
    if (err) {
        return err;
    }
    empty();
    o.pulling = true;
    return BTC_OK;
}

// ---- the server ----

static void on_server_header(void *user, uint8_t id, uint16_t total, uint16_t off, const uint8_t *data, uint16_t len) {
    (void)user;
    if (o.refusing) {
        return;
    }
    switch (id) {
        case OBEX_HEADER_NAME:
            if (off + len <= sizeof(o.name16)) {
                memcpy(o.name16 + off, data, len);
                o.name16_len = off + len;
            }
            if (off + len == total) {
                from_utf16(o.name16, o.name16_len, o.name, sizeof(o.name));
            }
            break;
        case OBEX_HEADER_TYPE:
            if (off + len < sizeof(o.type)) {
                memcpy(o.type + off, data, len);
                o.type[off + len] = 0;
            }
            break;
        case OBEX_HEADER_BODY:
        case OBEX_HEADER_END_OF_BODY:
            gather(data, len);
            break;
        default:
            break;
    }
}

static void server_reply(uint8_t code) {
    o.sresponse = code;
    goep_server_request_can_send_now(o.scid);
}

static void server_request(void) {
    obex_parser_operation_info_t info;
    obex_parser_get_operation_info(&o.sparser, &info);
    uint8_t op = info.opcode;
    bool final = (op & OBEX_OPCODE_FINAL_BIT_MASK) != 0;
    switch (op & ~OBEX_OPCODE_FINAL_BIT_MASK) {
        case OBEX_OPCODE_CONNECT & ~OBEX_OPCODE_FINAL_BIT_MASK:
            o.sconnect_reply = true;
            server_reply(OBEX_RESP_SUCCESS);
            break;
        case OBEX_OPCODE_DISCONNECT & ~OBEX_OPCODE_FINAL_BIT_MASK:
            server_reply(OBEX_RESP_SUCCESS);
            break;
        case OBEX_OPCODE_PUT:
            if (!final) {
                server_reply(o.refusing ? OBEX_RESP_SERVICE_UNAVAILABLE : OBEX_RESP_CONTINUE);
                break;
            }
            if (o.refusing) {
                o.refused++;
                server_reply(OBEX_RESP_SERVICE_UNAVAILABLE);
            } else {
                o.held = true;
                o.received++;
                o.changes |= BTC_CHANGED_PUSH;
                btc_log("object push: took %u bytes", (unsigned)o.len);
                server_reply(OBEX_RESP_SUCCESS);
            }
            o.in_put = false;
            break;
        case OBEX_OPCODE_ABORT & ~OBEX_OPCODE_FINAL_BIT_MASK:
            if (o.in_put && !o.refusing) {
                empty();
            }
            o.in_put = false;
            server_reply(OBEX_RESP_SUCCESS);
            break;
        default:
            server_reply(OBEX_RESP_NOT_IMPLEMENTED);
            break;
    }
}

static void on_server(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == GOEP_DATA_PACKET) {
        if (!o.sparsing) {
            // A new request: a PUT's first packet decides if it's taken.
            bool put = size > 0 && (packet[0] & ~OBEX_OPCODE_FINAL_BIT_MASK) == OBEX_OPCODE_PUT;
            if (put && !o.in_put) {
                o.in_put = true;
                o.refusing = o.held || !o.started || (o.cs != C_IDLE && o.pulling);
                if (!o.refusing) {
                    empty();
                    o.name16_len = 0;
                    o.changes |= BTC_CHANGED_PUSH;
                }
            }
            obex_parser_init_for_request(&o.sparser, on_server_header, NULL);
            o.sparsing = true;
        }
        if (obex_parser_process_data(&o.sparser, packet, size) != OBEX_PARSER_OBJECT_STATE_INCOMPLETE) {
            o.sparsing = false;
            server_request();
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_GOEP_META) {
        return;
    }
    switch (hci_event_goep_meta_get_subevent_code(packet)) {
        case GOEP_SUBEVENT_INCOMING_CONNECTION: {
            uint16_t cid = goep_subevent_incoming_connection_get_goep_cid(packet);
            if (o.listening && !o.sconnected) {
                goep_server_accept_connection(cid);
            } else {
                goep_server_decline_connection(cid);
            }
            break;
        }
        case GOEP_SUBEVENT_CONNECTION_OPENED:
            if (goep_subevent_connection_opened_get_status(packet) == ERROR_CODE_SUCCESS) {
                o.scid = goep_subevent_connection_opened_get_goep_cid(packet);
                o.sconnected = true;
                o.sparsing = o.in_put = false;
                goep_subevent_connection_opened_get_bd_addr(packet, o.peer);
                o.changes |= BTC_CHANGED_CONNECTION;
            }
            break;
        case GOEP_SUBEVENT_CONNECTION_CLOSED:
            if (o.in_put && !o.refusing) {
                empty();        // the sender went before the last packet
            }
            o.sconnected = o.sparsing = o.in_put = o.refusing = false;
            o.scid = 0;
            o.changes |= BTC_CHANGED_CONNECTION;
            break;
        case GOEP_SUBEVENT_CAN_SEND_NOW:
            if (o.sresponse == 0) {
                break;      // nothing waiting to go
            }
            if (o.sconnect_reply) {
                o.sconnect_reply = false;
                goep_server_response_create_connect(o.scid, OBEX_VERSION, 0,
                    goep_server_response_get_max_message_size(o.scid));
            } else {
                goep_server_response_create_general(o.scid);
            }
            goep_server_execute(o.scid, o.sresponse);
            o.sresponse = 0;
            break;
        default:
            break;
    }
}

// Object Push's SDP record: RFCOMM, any format.
static void make_record(uint8_t *rec, uint32_t handle) {
    memset(rec, 0, sizeof(sdp_opp));
    de_create_sequence(rec);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_RECORD_HANDLE);
    de_add_number(rec, DE_UINT, DE_SIZE_32, handle);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SERVICE_CLASS_ID_LIST);
    uint8_t *a = de_push_sequence(rec);
    de_add_number(a, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
    de_pop_sequence(rec, a);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PROTOCOL_DESCRIPTOR_LIST);
    a = de_push_sequence(rec);
    {
        uint8_t *l = de_push_sequence(a);
        de_add_number(l, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_L2CAP);
        de_pop_sequence(a, l);
        uint8_t *r = de_push_sequence(a);
        de_add_number(r, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_RFCOMM);
        de_add_number(r, DE_UINT, DE_SIZE_8, OPP_RFCOMM_CHANNEL);
        de_pop_sequence(a, r);
        uint8_t *x = de_push_sequence(a);
        de_add_number(x, DE_UUID, DE_SIZE_16, BLUETOOTH_PROTOCOL_OBEX);
        de_pop_sequence(a, x);
    }
    de_pop_sequence(rec, a);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BROWSE_GROUP_LIST);
    a = de_push_sequence(rec);
    de_add_number(a, DE_UUID, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
    de_pop_sequence(rec, a);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_BLUETOOTH_PROFILE_DESCRIPTOR_LIST);
    a = de_push_sequence(rec);
    {
        uint8_t *p = de_push_sequence(a);
        de_add_number(p, DE_UUID, DE_SIZE_16, BLUETOOTH_SERVICE_CLASS_OBEX_OBJECT_PUSH);
        de_add_number(p, DE_UINT, DE_SIZE_16, 0x0102);
        de_pop_sequence(a, p);
    }
    de_pop_sequence(rec, a);
    static const char name[] = "OBEX Object Push";
    de_add_number(rec, DE_UINT, DE_SIZE_16, 0x0100);
    de_add_data(rec, DE_STRING, sizeof(name) - 1, (uint8_t *)name);
    de_add_number(rec, DE_UINT, DE_SIZE_16, BLUETOOTH_ATTRIBUTE_SUPPORTED_FORMATS_LIST);
    a = de_push_sequence(rec);
    de_add_number(a, DE_UINT, DE_SIZE_8, 0xff);
    de_pop_sequence(rec, a);
}

int btc_opp_listen(bool on) {
    if (!o.started) {
        return BTC_ERR_STATE;
    }
    if (on == o.listening) {
        return BTC_OK;
    }
    // GOEP's server can't take a service back, so it stays registered
    // while the radio is on, and turns devices away while not listening.
    if (on) {
        if (!o.server_up) {
            btc_rfcomm_init_once();
            goep_server_init();
            uint8_t s = goep_server_register_service(&on_server, OPP_RFCOMM_CHANNEL, 0xffff, 0, 0, LEVEL_2);
            if (s != ERROR_CODE_SUCCESS) {
                btc_log("object push: no server, 0x%02x", s);
                return BTC_ERR_FAIL;
            }
            o.server_up = true;
        }
        o.sdp_handle = sdp_create_service_record_handle();
        make_record(sdp_opp, o.sdp_handle);
        sdp_register_service(sdp_opp);
    } else {
        sdp_unregister_service(o.sdp_handle);
        o.sdp_handle = 0;
    }
    o.listening = on;
    btc_radio_profile(BTC_PROFILE_PUSH, on, 0);
    return BTC_OK;
}

// ---- start and stop ----

int btc_opp_start(size_t max_bytes) {
    if (o.started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (max_bytes < 1024) {
        return BTC_ERR_ARG;
    }
    bool server_up = o.server_up;
    memset(&o, 0, sizeof(o));
    o.server_up = server_up;
    o.max = max_bytes;
    btc_goep_init_once();
    o.started = true;
    return BTC_OK;
}

void btc_opp_stop(void) {
    if (!o.started) {
        return;
    }
    btc_opp_listen(false);
    if (o.cs != C_IDLE && o.ccid) {
        goep_client_disconnect(o.ccid);
    }
    if (o.out) {
        btc_btstack_free(o.out);
        o.out = NULL;
    }
    empty();
    o.started = false;
}

bool btc_opp_started(void) {
    return o.started;
}

void btc_opp_radio_stopping(void) {
    btc_opp_stop();
    free_ertm();
    btc_opp_radio_lost();
}

void btc_opp_radio_lost(void) {
    if (o.server_up) {
        goep_server_deinit();
    }
    o.server_up = false;
    o.started = o.listening = o.sconnected = false;
    o.cs = C_IDLE;
    // The heap went with a soft reset; stopping freed it before.
    o.buf = o.out = o.ertm_raw = o.ertm = NULL;
    o.len = o.cap = 0;
}

// ---- results ----

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

void btc_opp_take(void) {
    empty();
}

void btc_opp_status(btc_opp_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = o.started;
    out->busy = o.cs != C_IDLE;
    out->listening = o.listening;
    out->receiving = o.sconnected && o.in_put && !o.refusing;
    out->held = o.held;
    memcpy(out->peer, o.peer, sizeof(out->peer));
    out->result = o.result;
    out->sent = o.sent;
    out->length = o.held ? (uint32_t)o.len : 0;
    out->truncated = o.truncated;
    out->received = o.received;
    out->refused = o.refused;
}

uint32_t btc_opp_changes(void) {
    uint32_t c = o.changes;
    o.changes = 0;
    return c;
}
