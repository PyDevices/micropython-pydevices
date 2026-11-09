// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's raw L2CAP channel on BTstack: one link at a time, as a server
// on a PSM others connect to or as a client of another device's. Bytes in
// and out through rings; each send is one packet of up to the peer's MTU.

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

#define TX_MAX 2048
#define LOCAL_MTU 1691      // what we take in one packet: BTstack's classic maximum

static bool started;
static uint32_t changed;
static btc_l2cap_status_t st;
static uint16_t cid;
static bool calling;

static uint8_t *rx;                 // rx_cap bytes, while started
static uint32_t rx_head, rx_count, rx_cap;
static uint8_t *tx;                 // TX_MAX bytes, while started
static uint32_t tx_head, tx_count;

static void tx_kick(void) {
    if (cid && tx_count) {
        l2cap_request_can_send_now_event(cid);
    }
}

static void tx_send(void) {
    if (!cid || !tx_count) {
        return;
    }
    static uint8_t buf[LOCAL_MTU];
    uint32_t n = tx_count;
    if (st.mtu && n > st.mtu) {
        n = st.mtu;
    }
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = tx[(tx_head + i) % TX_MAX];
    }
    if (l2cap_send(cid, buf, (uint16_t)n) == ERROR_CODE_SUCCESS) {
        tx_head = (tx_head + n) % TX_MAX;
        tx_count -= n;
        st.tx_bytes += n;
    }
    tx_kick();
}

static void on_l2cap(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    if (packet_type == L2CAP_DATA_PACKET) {
        if (channel != cid) {
            return;
        }
        for (uint16_t i = 0; i < size; i++) {
            if (rx && rx_count < rx_cap) {
                rx[(rx_head + rx_count) % rx_cap] = packet[i];
                rx_count++;
            } else {
                st.rx_dropped++;
            }
        }
        st.rx_bytes += size;
        changed |= BTC_CHANGED_L2CAP;
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case L2CAP_EVENT_INCOMING_CONNECTION: {
            uint16_t c = l2cap_event_incoming_connection_get_local_cid(packet);
            if (cid || calling) {
                l2cap_decline_connection(c);
            } else {
                l2cap_accept_connection(c);
            }
            break;
        }
        case L2CAP_EVENT_CHANNEL_OPENED: {
            uint8_t status = l2cap_event_channel_opened_get_status(packet);
            l2cap_event_channel_opened_get_address(packet, addr);
            btc_log("l2cap: open %s status 0x%02x", bd_addr_to_str(addr), status);
            calling = false;
            if (status == ERROR_CODE_SUCCESS) {
                cid = l2cap_event_channel_opened_get_local_cid(packet);
                st.mtu = l2cap_event_channel_opened_get_remote_mtu(packet);
                memcpy(st.peer, addr, 6);
                st.connected = true;
                tx_kick();
            }
            changed |= BTC_CHANGED_CONNECTION;
            break;
        }
        case L2CAP_EVENT_CAN_SEND_NOW:
            tx_send();
            break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            if (l2cap_event_channel_closed_get_local_cid(packet) != cid) {
                break;
            }
            cid = 0;
            st.connected = false;
            st.mtu = 0;
            memset(st.peer, 0, sizeof(st.peer));
            tx_head = tx_count = 0;
            changed |= BTC_CHANGED_CONNECTION;
            break;
        default:
            break;
    }
}

int btc_l2cap_start(uint16_t psm, size_t ring_bytes) {
    if (started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    rx_cap = ring_bytes ? ring_bytes : 4096;
    rx = btc_btstack_alloc(rx_cap);
    tx = btc_btstack_alloc(TX_MAX);
    if (rx == NULL || tx == NULL) {
        btc_btstack_free(rx);
        btc_btstack_free(tx);
        rx = tx = NULL;
        return BTC_ERR_NOMEM;
    }
    rx_head = rx_count = tx_head = tx_count = 0;
    memset(&st, 0, sizeof(st));
    changed = 0;
    calling = false;
    if (psm && l2cap_register_service(on_l2cap, psm, LOCAL_MTU, LEVEL_2) != ERROR_CODE_SUCCESS) {
        btc_btstack_free(rx);
        btc_btstack_free(tx);
        rx = tx = NULL;
        return BTC_ERR_FAIL;
    }
    st.psm = psm;
    st.listening = psm != 0;
    started = true;
    return BTC_OK;
}

void btc_l2cap_stop(void) {
    if (!started) {
        return;
    }
    if (cid) {
        l2cap_disconnect(cid);
        cid = 0;
    }
    if (st.psm) {
        l2cap_unregister_service(st.psm);
    }
    started = false;
    calling = false;
    st.listening = st.connected = false;
    btc_btstack_free(rx);
    btc_btstack_free(tx);
    rx = tx = NULL;
    rx_count = tx_count = 0;
}

void btc_l2cap_radio_stopping(void) {
    btc_l2cap_stop();
}

void btc_l2cap_radio_lost(void) {
    started = false;
    calling = false;
    rx = tx = NULL;
    rx_count = tx_count = 0;
    cid = 0;
    memset(&st, 0, sizeof(st));
}

bool btc_l2cap_started(void) {
    return started;
}

bool btc_l2cap_connected(void) {
    return cid != 0;
}

bool btc_l2cap_calling(void) {
    return calling;
}

int btc_l2cap_connect(const uint8_t *addr, uint16_t psm) {
    if (!started || cid || calling) {
        return BTC_ERR_STATE;
    }
    bd_addr_t a;
    memcpy(a, addr, 6);
    uint16_t c;
    uint8_t s = l2cap_create_channel(on_l2cap, a, psm, LOCAL_MTU, &c);
    btc_log("l2cap: calling %s psm 0x%04x: 0x%02x", bd_addr_to_str(a), psm, s);
    if (s != ERROR_CODE_SUCCESS) {
        return BTC_ERR_FAIL;
    }
    calling = true;
    return BTC_OK;
}

int btc_l2cap_disconnect(void) {
    if (!cid) {
        return BTC_ERR_STATE;
    }
    l2cap_disconnect(cid);
    return BTC_OK;
}

size_t btc_l2cap_any(void) {
    return rx_count;
}

size_t btc_l2cap_read(uint8_t *dst, size_t size) {
    size_t n = size < rx_count ? size : rx_count;
    for (size_t i = 0; i < n; i++) {
        dst[i] = rx[rx_head];
        rx_head = (rx_head + 1) % rx_cap;
    }
    rx_count -= n;
    return n;
}

size_t btc_l2cap_write(const uint8_t *src, size_t size) {
    if (!cid) {
        return 0;
    }
    size_t n = 0;
    while (n < size && tx_count < TX_MAX) {
        tx[(tx_head + tx_count) % TX_MAX] = src[n++];
        tx_count++;
    }
    tx_kick();
    return n;
}

size_t btc_l2cap_write_room(void) {
    return cid && tx ? TX_MAX - tx_count : 0;
}

uint32_t btc_l2cap_changes(void) {
    uint32_t c = changed;
    changed = 0;
    return c;
}

void btc_l2cap_status(btc_l2cap_status_t *out) {
    *out = st;
}
