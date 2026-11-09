// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The phone's library on BTstack's AVRCP browsing controller: its players,
// their folders and tracks, the queue and a search, over the browsing
// channel beside the sink's AVRCP link. See btclassic.h.
//
// One request runs at a time. The items of a reply come one by one; each is
// kept as it came (its type, a length and its body), in one buffer from the
// heap, for the binding to read.

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

#define ERTM_BYTES 8192

// Browsing runs over ERTM; the buffer holds its segments, and BTstack wants
// it on a 16-byte boundary.
static l2cap_ertm_config_t ertm_config = { 1, 2, 2000, 12000, 1000, 2, 2, 1 };

static struct {
    bool started;
    bool connecting;
    bool connected;
    bool busy;
    uint8_t op;
    uint16_t cid;
    btc_addr_t peer;
    int result;
    uint32_t changes;
    uint16_t uid_counter;
    uint16_t player;
    int depth;              // folders below the player's root, or BTC_UNKNOWN
    int pending_depth;      // what depth becomes when the change of folder lands

    uint8_t *ertm_raw;
    uint8_t *ertm;

    uint8_t *buf;
    size_t len, cap;
    bool truncated;
    uint16_t items;
} b;

static void finish(int result) {
    b.result = result;
    b.busy = false;
    if (result == BTC_OK && b.op == BTC_LIB_OP_PATH) {
        b.depth = b.pending_depth;
    }
    b.op = BTC_LIB_OP_NONE;
    b.changes |= BTC_CHANGED_LIBRARY;
}

// The channel's buffer outlives a stop until the link is down: BTstack
// writes into it while the channel closes.
static void free_ertm_if_stopped(void) {
    if (!b.started && !b.cid && !b.connecting) {
        btc_btstack_free(b.ertm_raw);
        b.ertm_raw = b.ertm = NULL;
    }
}

static void forget_link(void) {
    b.cid = 0;
    b.connected = b.connecting = false;
    if (b.busy) {
        finish(BTC_ERR_STATE);
    }
    b.depth = BTC_UNKNOWN;
}

static void keep(const uint8_t *item, uint16_t size) {
    if (size > 256) {
        size = 256;     // the length byte holds 255 after the type
    }
    if (b.len + 2 + size > b.cap) {
        b.truncated = true;
        return;
    }
    b.buf[b.len++] = item[0];          // the item's type
    b.buf[b.len++] = (uint8_t)(size - 1);
    memcpy(b.buf + b.len, item + 1, size - 1);
    b.len += size - 1;
    b.items++;
}

void btc_lib_on_browse(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == AVRCP_BROWSING_DATA_PACKET) {
        if (b.busy && size) {
            keep(packet, size);
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET || hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) {
        return;
    }
    switch (packet[2]) {
        case AVRCP_SUBEVENT_INCOMING_BROWSING_CONNECTION: {
            uint16_t cid = avrcp_subevent_incoming_browsing_connection_get_browsing_cid(packet);
            if (b.started && b.ertm && !b.connected && !b.cid) {
                avrcp_browsing_configure_incoming_connection(cid, b.ertm, ERTM_BYTES, &ertm_config);
                b.connecting = true;
            } else {
                avrcp_browsing_decline_incoming_connection(cid);
            }
            return;
        }
        case AVRCP_SUBEVENT_BROWSING_CONNECTION_ESTABLISHED: {
            uint8_t status = avrcp_subevent_browsing_connection_established_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                btc_log("library: open failed 0x%02x", status);
                forget_link();
                b.result = BTC_ERR_FAIL;
                b.changes |= BTC_CHANGED_CONNECTION;
                free_ertm_if_stopped();
                return;
            }
            b.cid = avrcp_subevent_browsing_connection_established_get_browsing_cid(packet);
            avrcp_subevent_browsing_connection_established_get_bd_addr(packet, b.peer);
            b.connecting = false;
            b.connected = true;
            b.result = BTC_OK;
            b.depth = BTC_UNKNOWN;
            b.changes |= BTC_CHANGED_CONNECTION;
            btc_log("library: open");
            return;
        }
        case AVRCP_SUBEVENT_BROWSING_CONNECTION_RELEASED:
            btc_log("library: closed");
            forget_link();
            b.changes |= BTC_CHANGED_CONNECTION;
            free_ertm_if_stopped();
            return;
        case AVRCP_SUBEVENT_BROWSING_DONE: {
            if (!b.busy) {
                return;
            }
            uint8_t status = avrcp_subevent_browsing_done_get_browsing_status(packet);
            uint8_t bt = avrcp_subevent_browsing_done_get_bluetooth_status(packet);
            b.uid_counter = avrcp_subevent_browsing_done_get_uid_counter(packet);
            if (bt != ERROR_CODE_SUCCESS) {
                finish(BTC_ERR_FAIL);
            } else if (status != AVRCP_BROWSING_ERROR_CODE_SUCCESS) {
                // AVRCP's own status codes, above the range of BTC_OK.
                finish(status);
            } else {
                if (b.op == BTC_LIB_OP_PLAYER) {
                    b.depth = b.items;      // a folder name for each level
                }
                finish(BTC_OK);
            }
            return;
        }
        default:
            return;
    }
}

// ---- the radio's hooks ----

void btc_lib_radio_stopping(void) {
    btc_lib_stop();
    btc_btstack_free(b.ertm_raw);   // the stack goes now, channel and all
    btc_lib_radio_lost();
}

void btc_lib_radio_lost(void) {
    b.started = b.connected = b.connecting = b.busy = false;
    b.cid = 0;
    b.ertm_raw = b.ertm = NULL;      // the heap went with a soft reset; stopping freed it before
    b.buf = NULL;
    b.len = b.cap = 0;
}

// ---- btclassic.h ----

int btc_lib_start(size_t max_bytes) {
    if (b.started) {
        return BTC_OK;
    }
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    b.ertm_raw = btc_btstack_alloc(ERTM_BYTES + 16);
    b.buf = btc_btstack_alloc(max_bytes);
    if (!b.ertm_raw || !b.buf) {
        btc_btstack_free(b.ertm_raw);
        btc_btstack_free(b.buf);
        b.ertm_raw = b.buf = NULL;
        return BTC_ERR_NOMEM;
    }
    b.ertm = (uint8_t *)(((uintptr_t)b.ertm_raw + 15) & ~(uintptr_t)15);
    b.cap = max_bytes;
    b.len = 0;
    b.started = true;
    b.changes = 0;
    b.result = BTC_OK;
    b.depth = BTC_UNKNOWN;
    return BTC_OK;
}

void btc_lib_stop(void) {
    if (!b.started) {
        return;
    }
    btc_lib_disconnect();
    if (b.busy) {
        finish(BTC_ERR_STATE);
    }
    b.started = false;
    btc_btstack_free(b.buf);
    b.buf = NULL;
    b.len = b.cap = 0;
    free_ertm_if_stopped();
}

bool btc_lib_started(void) {
    return b.started;
}

int btc_lib_connect(void) {
    if (!b.started) {
        return BTC_ERR_STATE;
    }
    if (b.connected || b.connecting) {
        return BTC_OK;
    }
    btc_sink_status_t sink;
    btc_sink_status(&sink);
    if (!sink.connected || !btc_sink_avrcp_cid()) {
        return BTC_ERR_STATE;
    }
    memcpy(b.peer, sink.peer, sizeof(b.peer));
    uint16_t cid;
    uint8_t s = avrcp_browsing_connect(b.peer, b.ertm, ERTM_BYTES, &ertm_config, &cid);
    if (s != ERROR_CODE_SUCCESS) {
        btc_log("library: connect 0x%02x", s);
        return BTC_ERR_FAIL;
    }
    b.connecting = true;
    return BTC_OK;
}

int btc_lib_disconnect(void) {
    if (!b.started) {
        return BTC_ERR_STATE;
    }
    if (b.cid) {
        avrcp_browsing_disconnect(b.cid);
    }
    return BTC_OK;
}

static int begin(uint8_t op) {
    if (!b.started || !b.connected) {
        return BTC_ERR_STATE;
    }
    if (b.busy) {
        return BTC_ERR_STATE;
    }
    b.op = op;
    b.busy = true;
    b.len = 0;
    b.items = 0;
    b.truncated = false;
    return BTC_OK;
}

static int sent(uint8_t s) {
    if (s != ERROR_CODE_SUCCESS) {
        b.busy = false;
        b.op = BTC_LIB_OP_NONE;
        return BTC_ERR_FAIL;
    }
    return BTC_OK;
}

int btc_lib_list(uint8_t scope, uint32_t start, uint32_t count) {
    if (scope > BTC_LIB_SCOPE_QUEUE || count == 0) {
        return BTC_ERR_ARG;
    }
    int r = begin(BTC_LIB_OP_LIST);
    if (r) {
        return r;
    }
    uint32_t end = start + count - 1;
    // Title is the item's own name; ask for the artist and album beside it.
    uint32_t attrs = (1u << AVRCP_MEDIA_ATTR_ARTIST) | (1u << AVRCP_MEDIA_ATTR_ALBUM);
    uint8_t s;
    switch (scope) {
        case BTC_LIB_SCOPE_PLAYERS:
            s = avrcp_browsing_controller_get_media_players(b.cid, start, end, AVRCP_MEDIA_ATTR_ALL);
            break;
        case BTC_LIB_SCOPE_FOLDER:
            s = avrcp_browsing_controller_browse_file_system(b.cid, start, end, attrs);
            break;
        case BTC_LIB_SCOPE_SEARCH:
            s = avrcp_browsing_controller_browse_media(b.cid, start, end, attrs);
            break;
        default:
            s = avrcp_browsing_controller_browse_now_playing_list(b.cid, start, end, attrs);
            break;
    }
    return sent(s);
}

int btc_lib_set_player(uint16_t player) {
    int r = begin(BTC_LIB_OP_PLAYER);
    if (r) {
        return r;
    }
    b.player = player;
    return sent(avrcp_browsing_controller_set_browsed_player(b.cid, player));
}

int btc_lib_enter(const uint8_t *uid) {
    int r = begin(BTC_LIB_OP_PATH);
    if (r) {
        return r;
    }
    b.pending_depth = b.depth < 0 ? BTC_UNKNOWN : b.depth + 1;
    return sent(avrcp_browsing_controller_go_down_one_level(b.cid, (uint8_t *)uid));
}

int btc_lib_up(void) {
    int r = begin(BTC_LIB_OP_PATH);
    if (r) {
        return r;
    }
    b.pending_depth = b.depth <= 0 ? b.depth : b.depth - 1;
    return sent(avrcp_browsing_controller_go_up_one_level(b.cid));
}

int btc_lib_search(const char *text) {
    int r = begin(BTC_LIB_OP_SEARCH);
    if (r) {
        return r;
    }
    return sent(avrcp_browsing_controller_search(b.cid, (uint16_t)strlen(text), (char *)text));
}

// Playing goes over the control channel, which the sink holds.
int btc_lib_play(uint8_t scope, const uint8_t *uid) {
    if (!b.started || !b.connected) {
        return BTC_ERR_STATE;
    }
    uint16_t cid = btc_sink_avrcp_cid();
    if (!cid) {
        return BTC_ERR_STATE;
    }
    avrcp_browsing_scope_t s = scope == BTC_LIB_SCOPE_SEARCH ? AVRCP_BROWSING_SEARCH
        : scope == BTC_LIB_SCOPE_QUEUE ? AVRCP_BROWSING_NOW_PLAYING
        : AVRCP_BROWSING_MEDIA_PLAYER_VIRTUAL_FILESYSTEM;
    uint8_t st = avrcp_controller_play_item_for_scope(cid, (uint8_t *)uid, b.uid_counter, s);
    return st == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

size_t btc_lib_data(uint8_t *dst, size_t size) {
    size_t n = b.len < size ? b.len : size;
    if (n) {
        memcpy(dst, b.buf, n);
    }
    return n;
}

void btc_lib_status(btc_lib_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = b.started;
    out->connecting = b.connecting;
    out->connected = b.connected;
    memcpy(out->peer, b.peer, sizeof(out->peer));
    out->busy = b.busy;
    out->result = b.result;
    out->length = b.len;
    out->items = b.items;
    out->truncated = b.truncated;
    out->player = b.player;
    out->depth = b.depth;
    out->uid_counter = b.uid_counter;
}

uint32_t btc_lib_changes(void) {
    uint32_t c = b.changes;
    b.changes = 0;
    return c;
}
