// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The message client on BTstack's GOEP: a phone's texts over the Message
// Access Profile. BTstack has no MAP client, so this speaks OBEX itself, as
// the Object Push code does. See btclassic.h.
//
// One session at a time to one message service. A request first walks the
// phone to its folder (SETPATH from the root, a step per component), then
// runs as a GET or PUT; its reply gathers into one heap buffer up to
// `max_bytes`. The phone's change reports (a notification server on this
// side) are not offered here: notify() raises.

#include <string.h>

#include "btstack.h"
#include "classic/goep_client.h"
#include "classic/obex.h"
#include "classic/obex_parser.h"

#include "btc_btstack.h"

#define MAP_ERTM_SIZE 4400
#define NAME_MAX_LEN 64
#define PATH_MAX_LEN 96
#define AP_MAX 40
#define MAP_FEATURES 0x0000003cu    // browsing, upload, delete, instance information

// Application parameter tags.
#define AP_MAX_LIST_COUNT 0x01
#define AP_LIST_START_OFFSET 0x02
#define AP_FILTER_MESSAGE_TYPE 0x03
#define AP_FILTER_READ_STATUS 0x06
#define AP_ATTACHMENT 0x0a
#define AP_RETRY 0x0c
#define AP_NEW_MESSAGE 0x0d
#define AP_PARAMETER_MASK 0x10
#define AP_FOLDER_LISTING_SIZE 0x11
#define AP_LISTING_SIZE 0x12
#define AP_SUBJECT_LENGTH 0x13
#define AP_CHARSET 0x14
#define AP_STATUS_INDICATOR 0x17
#define AP_STATUS_VALUE 0x18
#define AP_MSE_TIME 0x19
#define AP_MAP_SUPPORTED_FEATURES 0x29

static const uint8_t mas_target[16] = {
    0xbb, 0x58, 0x2b, 0x40, 0x42, 0x0c, 0x11, 0xdb, 0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

typedef enum {
    C_IDLE = 0,
    C_W4_GOEP,          // the bearer is opening
    C_W2_CONNECT,
    C_W4_CONNECT,
    C_READY,            // connected, no request running
    C_W2_REQUEST,       // the next GET or PUT packet
    C_W4_REQUEST,
    C_W2_SETPATH,
    C_W4_SETPATH,
    C_W2_DISCONNECT,
    C_W4_DISCONNECT,
    C_W4_CLOSE,         // the bearer is closing
} cstate_t;

typedef enum {
    OP_NONE = 0,
    OP_FOLDERS,         // GET x-obex/folder-listing
    OP_LIST,            // GET x-bt/MAP-msg-listing
    OP_GET,             // GET x-bt/message
    OP_STATUS,          // PUT x-bt/messageStatus
    OP_PUSH,            // PUT x-bt/message
    OP_UPDATE,          // PUT x-bt/MAP-messageUpdate
} op_t;

static struct {
    bool started;
    bool connected;
    bool connecting;
    uint32_t changes;

    cstate_t cs;
    goep_client_t client;
    uint16_t cid;
    btc_addr_t peer;
    obex_parser_t parser;
    uint8_t *ertm_raw, *ertm;
    l2cap_ertm_config_t ertm_config;
    uint8_t conn_id_buf[4];
    int instance;
    uint32_t features, types;

    // The request.
    bool busy;
    op_t op;
    char folder[PATH_MAX_LEN];      // where the phone has to be
    char cwd[PATH_MAX_LEN];         // where it is
    bool cwd_known;
    char name[NAME_MAX_LEN];
    uint8_t ap[AP_MAX];
    uint8_t ap_len;
    int walk;                       // -1 at the root step, else the component
    bool first;                     // the first packet of the request
    uint8_t *body;                  // a PUT's, a copy
    size_t body_len, body_off;
    uint8_t reply_ap[48];           // the reply's application parameters

    // The reply.
    int result;
    bool truncated;
    int listing_size, new_message;
    char mse_time[24];
    char pushed[NAME_MAX_LEN];
    uint8_t *buf;
    size_t len, cap, max;
} m;

// ---- the buffer ----

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
        uint8_t *nb = btc_btstack_alloc(cap);
        if (nb == NULL) {
            m.truncated = true;
            return;
        }
        if (m.buf) {
            memcpy(nb, m.buf, m.len);
            btc_btstack_free(m.buf);
        }
        m.buf = nb;
        m.cap = cap;
    }
    memcpy(m.buf + m.len, data, n);
    m.len += n;
}

// ---- small helpers ----

static void ap8(uint8_t tag, uint8_t v) {
    m.ap[m.ap_len++] = tag;
    m.ap[m.ap_len++] = 1;
    m.ap[m.ap_len++] = v;
}

static void ap16(uint8_t tag, uint16_t v) {
    m.ap[m.ap_len++] = tag;
    m.ap[m.ap_len++] = 2;
    m.ap[m.ap_len++] = v >> 8;
    m.ap[m.ap_len++] = v & 0xff;
}

static void ap32(uint8_t tag, uint32_t v) {
    m.ap[m.ap_len++] = tag;
    m.ap[m.ap_len++] = 4;
    m.ap[m.ap_len++] = v >> 24;
    m.ap[m.ap_len++] = v >> 16;
    m.ap[m.ap_len++] = v >> 8;
    m.ap[m.ap_len++] = v & 0xff;
}

static int obex_result(uint8_t code) {
    code &= 0x7f;
    return code == (OBEX_RESP_SUCCESS & 0x7f) ? BTC_OK : code | 0x80;
}

// The application parameters that ride on a reply.
static void reply_params(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 2 <= n;) {
        uint8_t tag = p[i], len = p[i + 1];
        const uint8_t *v = p + i + 2;
        if (i + 2 + len > n) {
            break;
        }
        if ((tag == AP_LISTING_SIZE || tag == AP_FOLDER_LISTING_SIZE) && len == 2) {
            m.listing_size = (v[0] << 8) | v[1];
        } else if (tag == AP_NEW_MESSAGE && len == 1) {
            m.new_message = v[0];
        } else if (tag == AP_MSE_TIME) {
            size_t k = len < sizeof(m.mse_time) - 1 ? len : sizeof(m.mse_time) - 1;
            memcpy(m.mse_time, v, k);
            m.mse_time[k] = 0;
        }
        i += 2 + len;
    }
}

// A UTF-16BE Name header as UTF-8 (the handles are ASCII).
static void name_from_utf16(const uint8_t *s, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i + 1 < n && k + 1 < cap; i += 2) {
        uint16_t c = (s[i] << 8) | s[i + 1];
        if (c == 0) {
            break;
        }
        out[k++] = c < 0x80 ? (char)c : '?';
    }
    out[k] = 0;
}

// ---- the session ----

static void free_ertm(void) {
    if (m.ertm_raw) {
        btc_btstack_free(m.ertm_raw);
    }
    m.ertm_raw = m.ertm = NULL;
}

static void request_done(int result) {
    m.result = result;
    m.busy = false;
    m.op = OP_NONE;
    if (m.body) {
        btc_btstack_free(m.body);
        m.body = NULL;
    }
    m.cs = m.connected ? C_READY : m.cs;
    m.changes |= BTC_CHANGED_MESSAGES;
}

static void client_next(cstate_t s) {
    m.cs = s;
    goep_client_request_can_send_now(m.cid);
}

static void on_header(void *user, uint8_t id, uint16_t total, uint16_t off, const uint8_t *data, uint16_t len) {
    (void)user;
    switch (id) {
        case OBEX_HEADER_CONNECTION_ID:
            if (obex_parser_header_store(m.conn_id_buf, sizeof(m.conn_id_buf), total, off, data, len)
                == OBEX_PARSER_HEADER_COMPLETE) {
                goep_client_set_connection_id(m.cid, big_endian_read_32(m.conn_id_buf, 0));
            }
            break;
        case OBEX_HEADER_BODY:
        case OBEX_HEADER_END_OF_BODY:
            if (m.op == OP_FOLDERS || m.op == OP_LIST || m.op == OP_GET) {
                gather(data, len);
            }
            break;
        case OBEX_HEADER_APPLICATION_PARAMETERS:
            if (obex_parser_header_store(m.reply_ap, sizeof(m.reply_ap), total, off, data, len)
                == OBEX_PARSER_HEADER_COMPLETE) {
                reply_params(m.reply_ap, total < sizeof(m.reply_ap) ? total : sizeof(m.reply_ap));
            }
            break;
        case OBEX_HEADER_NAME:
            if (m.op == OP_PUSH) {
                name_from_utf16(data, len, m.pushed, sizeof(m.pushed));
            }
            break;
        default:
            break;
    }
}

static void expect(uint8_t opcode) {
    obex_parser_init_for_response(&m.parser, opcode, on_header, NULL);
}

static const char *op_type(op_t op) {
    switch (op) {
        case OP_FOLDERS: return "x-obex/folder-listing";
        case OP_LIST: return "x-bt/MAP-msg-listing";
        case OP_GET:
        case OP_PUSH: return "x-bt/message";
        case OP_STATUS: return "x-bt/messageStatus";
        case OP_UPDATE: return "x-bt/MAP-messageUpdate";
        default: return "";
    }
}

// The walk to the request's folder: the root, then a step per component.
// Returns the component for step `walk`, or false when the walk is over.
static bool walk_part(char *part, size_t cap) {
    const char *s = m.folder;
    for (int i = 0; i < m.walk && s; i++) {
        s = strchr(s, '/');
        if (s) {
            s++;
        }
    }
    if (s == NULL || *s == 0) {
        return false;
    }
    const char *e = strchr(s, '/');
    size_t n = e ? (size_t)(e - s) : strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(part, s, n);
    part[n] = 0;
    return true;
}

static void send_request_packet(void) {
    bool put = m.op == OP_STATUS || m.op == OP_PUSH || m.op == OP_UPDATE;
    if (!put) {
        goep_client_request_create_get(m.cid);
        if (m.first) {
            goep_client_header_add_type(m.cid, op_type(m.op));
            if (m.op != OP_FOLDERS && m.name[0]) {
                goep_client_header_add_name(m.cid, m.name);
            }
            if (m.ap_len) {
                goep_client_header_add_application_parameters(m.cid, m.ap, m.ap_len);
            }
            m.first = false;
        }
        m.cs = C_W4_REQUEST;
        expect(OBEX_OPCODE_GET);
        goep_client_execute(m.cid);
        return;
    }
    goep_client_request_create_put(m.cid);
    if (m.first) {
        goep_client_header_add_type(m.cid, op_type(m.op));
        if (m.op != OP_UPDATE) {
            goep_client_header_add_name(m.cid, m.name);
        }
        if (m.ap_len) {
            goep_client_header_add_application_parameters(m.cid, m.ap, m.ap_len);
        }
        m.first = false;
    }
    uint16_t room = goep_client_request_get_max_body_size(m.cid);
    uint32_t n = room > 3 ? room - 3u : 0;
    if (n > m.body_len - m.body_off) {
        n = m.body_len - m.body_off;
    }
    bool final = m.body_off + n >= m.body_len;
    goep_client_header_add_variable(m.cid, final ? OBEX_HEADER_END_OF_BODY : OBEX_HEADER_BODY,
        m.body + m.body_off, (uint16_t)n);
    m.body_off += n;
    m.cs = C_W4_REQUEST;
    expect(OBEX_OPCODE_PUT);
    goep_client_execute_with_final_bit(m.cid, final);
}

static void client_send(void) {
    char part[NAME_MAX_LEN];
    switch (m.cs) {
        case C_W2_CONNECT: {
            goep_client_request_create_connect(m.cid, OBEX_VERSION, 0, OBEX_MAX_PACKETLEN_DEFAULT);
            goep_client_header_add_target(m.cid, mas_target, sizeof(mas_target));
            uint8_t ap[6] = { AP_MAP_SUPPORTED_FEATURES, 4, MAP_FEATURES >> 24, MAP_FEATURES >> 16,
                              MAP_FEATURES >> 8, MAP_FEATURES & 0xff };
            goep_client_header_add_application_parameters(m.cid, ap, sizeof(ap));
            m.cs = C_W4_CONNECT;
            expect(OBEX_OPCODE_CONNECT);
            goep_client_execute(m.cid);
            break;
        }
        case C_W2_SETPATH:
            goep_client_request_create_set_path(m.cid, 1 << 1);     // don't create it
            if (m.walk < 0) {
                goep_client_header_add_name(m.cid, "");
            } else if (walk_part(part, sizeof(part))) {
                goep_client_header_add_name_prefix(m.cid, part, strlen(part));
            }
            m.cs = C_W4_SETPATH;
            expect(OBEX_OPCODE_SETPATH);
            goep_client_execute(m.cid);
            break;
        case C_W2_REQUEST:
            send_request_packet();
            break;
        case C_W2_DISCONNECT:
            goep_client_request_create_disconnect(m.cid);
            m.cs = C_W4_DISCONNECT;
            expect(OBEX_OPCODE_DISCONNECT);
            goep_client_execute(m.cid);
            break;
        default:
            break;
    }
}

// Start the request in its folder: straight away, or after a walk.
static void begin_request(void) {
    if (m.cwd_known && strcmp(m.cwd, m.folder) == 0) {
        m.first = true;
        client_next(C_W2_REQUEST);
        return;
    }
    m.cwd_known = false;
    m.walk = -1;
    client_next(C_W2_SETPATH);
}

static void client_response(void) {
    obex_parser_operation_info_t info;
    obex_parser_get_operation_info(&m.parser, &info);
    uint8_t code = info.response_code;
    char part[NAME_MAX_LEN];
    switch (m.cs) {
        case C_W4_CONNECT:
            if (code == OBEX_RESP_SUCCESS) {
                m.connected = true;
                m.connecting = false;
                m.cs = C_READY;
                m.changes |= BTC_CHANGED_CONNECTION;
                btc_log("messages connected: instance %d, features 0x%x, types 0x%x",
                    m.instance, (unsigned)m.features, (unsigned)m.types);
            } else {
                m.connecting = false;
                m.result = obex_result(code);
                m.changes |= BTC_CHANGED_CONNECTION;
                m.cs = C_W2_DISCONNECT;     // closes the bearer
                goep_client_disconnect(m.cid);
                m.cs = C_W4_CLOSE;
            }
            break;
        case C_W4_SETPATH:
            if (code != OBEX_RESP_SUCCESS) {
                m.cwd_known = false;
                request_done(obex_result(code));
                break;
            }
            m.walk++;
            if (walk_part(part, sizeof(part))) {
                client_next(C_W2_SETPATH);
            } else {
                strcpy(m.cwd, m.folder);
                m.cwd_known = true;
                m.first = true;
                client_next(C_W2_REQUEST);
            }
            break;
        case C_W4_REQUEST:
            if (code == OBEX_RESP_CONTINUE) {
                client_next(C_W2_REQUEST);
            } else {
                request_done(obex_result(code));
            }
            break;
        case C_W4_DISCONNECT:
            m.cs = C_W4_CLOSE;
            goep_client_disconnect(m.cid);
            break;
        default:
            break;
    }
}

static void on_client(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == GOEP_DATA_PACKET) {
        if (obex_parser_process_data(&m.parser, packet, size) == OBEX_PARSER_OBJECT_STATE_COMPLETE) {
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
                btc_log("messages didn't connect: 0x%02x", status);
                m.cs = C_IDLE;
                m.connecting = false;
                m.result = BTC_ERR_FAIL;
                free_ertm();
                m.changes |= BTC_CHANGED_CONNECTION;
                break;
            }
            m.cid = goep_subevent_connection_opened_get_goep_cid(packet);
            m.features = goep_client_get_map_supported_features(m.cid);
            m.types = goep_client_get_map_supported_message_types(m.cid);
            m.instance = goep_client_get_map_mas_instance_id(m.cid);
            client_next(C_W2_CONNECT);
            break;
        }
        case GOEP_SUBEVENT_CAN_SEND_NOW:
            client_send();
            break;
        case GOEP_SUBEVENT_CONNECTION_CLOSED: {
            btc_log("messages closed");
            bool was = m.connected || m.connecting;
            m.connected = m.connecting = false;
            m.cs = C_IDLE;
            m.cid = 0;
            m.cwd_known = false;
            free_ertm();
            if (m.busy) {
                request_done(BTC_ERR_STATE);
            }
            if (was) {
                m.changes |= BTC_CHANGED_CONNECTION;
            }
            break;
        }
        default:
            break;
    }
}

// ---- start and stop ----

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
    m.instance = BTC_UNKNOWN;
    btc_goep_init_once();
    m.started = true;
    return BTC_OK;
}

void btc_msg_stop(void) {
    if (!m.started) {
        return;
    }
    if (m.cs != C_IDLE && m.cid) {
        goep_client_disconnect(m.cid);
    }
    btc_msg_release();
    if (m.body) {
        btc_btstack_free(m.body);
        m.body = NULL;
    }
    free_ertm();
    m.started = false;
    m.busy = m.connected = m.connecting = false;
}

bool btc_msg_started(void) {
    return m.started;
}

void btc_map_radio_stopping(void) {
    btc_msg_stop();
}

void btc_map_radio_lost(void) {
    m.started = m.connected = m.connecting = m.busy = false;
    m.cs = C_IDLE;
    m.buf = NULL;       // the heap went with a soft reset; stopping freed it before
    m.body = NULL;
    m.ertm_raw = m.ertm = NULL;
    m.len = m.cap = 0;
}

int btc_msg_connect(const uint8_t *addr, int instance) {
    if (!m.started) {
        return BTC_ERR_STATE;
    }
    if (m.connected || m.connecting) {
        return BTC_OK;
    }
    if (m.cs != C_IDLE) {
        return BTC_ERR_STATE;
    }
    if (addr) {
        memcpy(m.peer, addr, sizeof(m.peer));
    } else {
        btc_hf_status_t hf;
        btc_sink_status_t sink;
        btc_hf_status(&hf);
        btc_sink_status(&sink);
        if (hf.connected) {
            memcpy(m.peer, hf.peer, sizeof(m.peer));
        } else if (sink.connected) {
            memcpy(m.peer, sink.peer, sizeof(m.peer));
        } else if (!btc_radio_last_peer(m.peer)) {
            btc_addr_t paired[1];
            if (btc_radio_paired(paired, 1) < 1) {
                return BTC_ERR_STATE;
            }
            memcpy(m.peer, paired[0], sizeof(m.peer));
        }
    }
    m.ertm_raw = btc_btstack_alloc(MAP_ERTM_SIZE + 16);
    if (m.ertm_raw == NULL) {
        return BTC_ERR_NOMEM;
    }
    m.ertm = (uint8_t *)(((uintptr_t)m.ertm_raw + 15) & ~(uintptr_t)15);
    m.ertm_config = (l2cap_ertm_config_t){ 1, 2, 2000, 12000, 1000, 2, 2, 1 };
    m.result = BTC_OK;
    m.connecting = true;
    m.cwd_known = false;
    m.cs = C_W4_GOEP;
    uint8_t s = goep_client_connect(&m.client, &m.ertm_config, m.ertm, MAP_ERTM_SIZE, &on_client, m.peer,
        BLUETOOTH_SERVICE_CLASS_MESSAGE_ACCESS_SERVER, instance < 0 ? 0 : (uint8_t)instance, &m.cid);
    if (s != ERROR_CODE_SUCCESS) {
        m.cs = C_IDLE;
        m.connecting = false;
        free_ertm();
        return BTC_ERR_FAIL;
    }
    return BTC_OK;
}

int btc_msg_disconnect(void) {
    if (!m.started || !m.connected) {
        return BTC_ERR_STATE;
    }
    if (m.cs == C_READY) {
        client_next(C_W2_DISCONNECT);
    } else {
        goep_client_disconnect(m.cid);
    }
    return BTC_OK;
}

// ---- requests ----

// Claim the one request slot, ready its fields and empty the reply.
static int begin(op_t op, const char *folder, const char *name) {
    if (!m.started || !m.connected || m.busy || m.cs != C_READY) {
        return BTC_ERR_STATE;
    }
    if (folder == NULL || strlen(folder) >= PATH_MAX_LEN || (name && strlen(name) >= NAME_MAX_LEN)) {
        return BTC_ERR_ARG;
    }
    m.busy = true;
    m.op = op;
    strcpy(m.folder, folder);
    strcpy(m.name, name ? name : "");
    m.ap_len = 0;
    m.len = 0;
    m.truncated = false;
    m.result = BTC_OK;
    m.pushed[0] = 0;
    m.body_len = m.body_off = 0;
    return BTC_OK;
}

// Folder "" in a request means: where the phone already is.
static void keep_folder(void) {
    if (m.cwd_known) {
        strcpy(m.folder, m.cwd);
    }
}

int btc_msg_folders(const char *folder, uint16_t offset, uint16_t count) {
    int err = begin(OP_FOLDERS, folder, NULL);
    if (err) {
        return err;
    }
    ap16(AP_MAX_LIST_COUNT, count);
    if (offset) {
        ap16(AP_LIST_START_OFFSET, offset);
    }
    begin_request();
    return BTC_OK;
}

int btc_msg_list(const char *folder, const char *name, uint16_t offset, uint16_t count,
    uint8_t read_filter, uint8_t type_filter, uint32_t parameter_mask, uint8_t subject_length) {
    int err = begin(OP_LIST, folder, name);
    if (err) {
        return err;
    }
    ap16(AP_MAX_LIST_COUNT, count);
    if (offset) {
        ap16(AP_LIST_START_OFFSET, offset);
    }
    if (read_filter) {
        ap8(AP_FILTER_READ_STATUS, read_filter);
    }
    if (type_filter) {
        ap8(AP_FILTER_MESSAGE_TYPE, type_filter);
    }
    if (parameter_mask) {
        ap32(AP_PARAMETER_MASK, parameter_mask);
    }
    if (subject_length) {
        ap8(AP_SUBJECT_LENGTH, subject_length);
    }
    begin_request();
    return BTC_OK;
}

int btc_msg_get(const char *handle, bool attachments, bool utf8) {
    if (handle == NULL) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_GET, "", handle);
    if (err) {
        return err;
    }
    // A message is fetched by handle from anywhere: keep the folder as is.
    keep_folder();
    ap8(AP_ATTACHMENT, attachments ? 1 : 0);
    ap8(AP_CHARSET, utf8 ? 1 : 0);
    begin_request();
    return BTC_OK;
}

int btc_msg_set_status(const char *handle, uint8_t indicator, bool value) {
    if (handle == NULL || indicator > 1) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_STATUS, "", handle);
    if (err) {
        return err;
    }
    keep_folder();
    ap8(AP_STATUS_INDICATOR, indicator);
    ap8(AP_STATUS_VALUE, value ? 1 : 0);
    // The spec asks for a one-byte filler body.
    m.body = btc_btstack_alloc(1);
    if (m.body == NULL) {
        m.busy = false;
        return BTC_ERR_NOMEM;
    }
    m.body[0] = '0';
    m.body_len = 1;
    begin_request();
    return BTC_OK;
}

int btc_msg_push(const char *folder, const char *name, const uint8_t *bmsg, size_t len, bool utf8, bool retry) {
    if (bmsg == NULL || len == 0) {
        return BTC_ERR_ARG;
    }
    int err = begin(OP_PUSH, folder, name);
    if (err) {
        return err;
    }
    ap8(AP_CHARSET, utf8 ? 1 : 0);
    ap8(AP_RETRY, retry ? 1 : 0);
    m.body = btc_btstack_alloc(len);
    if (m.body == NULL) {
        m.busy = false;
        return BTC_ERR_NOMEM;
    }
    memcpy(m.body, bmsg, len);
    m.body_len = len;
    begin_request();
    return BTC_OK;
}

int btc_msg_update_inbox(void) {
    int err = begin(OP_UPDATE, "", NULL);
    if (err) {
        return err;
    }
    keep_folder();
    m.body = btc_btstack_alloc(1);
    if (m.body == NULL) {
        m.busy = false;
        return BTC_ERR_NOMEM;
    }
    m.body[0] = '0';
    m.body_len = 1;
    begin_request();
    return BTC_OK;
}

int btc_msg_notify(bool on) {
    // The phone's reports come to a server on this side, which isn't built.
    (void)on;
    return BTC_ERR_UNSUPPORTED;
}

size_t btc_msg_data(uint8_t *dst, size_t size) {
    if (m.busy || m.buf == NULL) {
        return 0;
    }
    size_t n = m.len < size ? m.len : size;
    memcpy(dst, m.buf, n);
    return n;
}

void btc_msg_release(void) {
    if (m.busy) {
        return;
    }
    if (m.buf) {
        btc_btstack_free(m.buf);
    }
    m.buf = NULL;
    m.len = m.cap = 0;
}

size_t btc_msg_event(uint8_t *dst, size_t size) {
    (void)dst;
    (void)size;
    return 0;
}

void btc_msg_status(btc_msg_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = m.started;
    out->connected = m.connected;
    out->connecting = m.connecting;
    memcpy(out->peer, m.peer, sizeof(out->peer));
    out->instance = m.connected ? m.instance : BTC_UNKNOWN;
    out->instances = m.connected ? 1 : 0;
    out->features = m.connected ? m.features : 0;
    out->types = m.connected ? m.types : 0;
    out->busy = m.busy;
    out->result = m.result;
    out->length = m.len;
    out->truncated = m.truncated;
    out->listing_size = m.listing_size;
    out->new_message = m.new_message;
    strcpy(out->mse_time, m.mse_time);
    strcpy(out->pushed, m.pushed);
}

uint32_t btc_msg_changes(void) {
    uint32_t c = m.changes;
    m.changes = 0;
    return c;
}
