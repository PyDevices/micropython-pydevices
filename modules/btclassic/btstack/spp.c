// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic's serial port on BTstack: SPP over RFCOMM, as a server others
// connect to (a PC's Bluetooth COM port, a phone's terminal app) or as a
// client of another board. One link at a time; bytes in and out through
// rings.

#include <string.h>

#include "btstack.h"

#include "btc_btstack.h"

#define SPP_CHANNEL 2
#define RX_MAX 4096
#define TX_MAX 2048

static bool started;
static uint32_t changed;
static btc_spp_status_t st;
static uint16_t cid;
static uint8_t sdp_spp[150];
static char service_name[32] = "Serial Port";

static uint8_t *rx;                 // rx_cap bytes, while started
static uint32_t rx_head, rx_count, rx_cap = RX_MAX;
static uint8_t *tx;                 // TX_MAX bytes, while started
static uint32_t tx_head, tx_count;

static void tx_kick(void) {
    if (cid && tx_count) {
        rfcomm_request_can_send_now_event(cid);
    }
}

static void tx_send(void) {
    if (!cid || !tx_count) {
        return;
    }
    uint8_t buf[512];
    uint16_t mtu = rfcomm_get_max_frame_size(cid);
    uint32_t n = tx_count;
    if (n > mtu) {
        n = mtu;
    }
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = tx[(tx_head + i) % TX_MAX];
    }
    if (rfcomm_send(cid, buf, n) == ERROR_CODE_SUCCESS) {
        tx_head = (tx_head + n) % TX_MAX;
        tx_count -= n;
        st.tx_bytes += n;
        if (tx_count == 0) {
            changed |= BTC_CHANGED_SERIAL;     // room to write again
        }
    }
    tx_kick();
}

static void on_rfcomm(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == RFCOMM_DATA_PACKET) {
        for (uint16_t i = 0; i < size; i++) {
            if (rx && rx_count < rx_cap) {
                rx[(rx_head + rx_count) % rx_cap] = packet[i];
                rx_count++;
            } else {
                st.rx_dropped++;
            }
        }
        st.rx_bytes += size;
        changed |= BTC_CHANGED_SERIAL;
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    bd_addr_t addr;
    switch (hci_event_packet_get_type(packet)) {
        case RFCOMM_EVENT_INCOMING_CONNECTION:
            rfcomm_event_incoming_connection_get_bd_addr(packet, addr);
            if (cid) {
                rfcomm_decline_connection(rfcomm_event_incoming_connection_get_rfcomm_cid(packet));
                break;
            }
            btc_log("spp: %s connecting", bd_addr_to_str(addr));
            rfcomm_accept_connection(rfcomm_event_incoming_connection_get_rfcomm_cid(packet));
            break;
        case RFCOMM_EVENT_CHANNEL_OPENED: {
            uint8_t status = rfcomm_event_channel_opened_get_status(packet);
            rfcomm_event_channel_opened_get_bd_addr(packet, addr);
            btc_log("spp: open %s status 0x%02x", bd_addr_to_str(addr), status);
            if (status != ERROR_CODE_SUCCESS) {
                break;
            }
            cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
            st.mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
            st.channel = rfcomm_event_channel_opened_get_server_channel(packet);
            memcpy(st.peer, addr, 6);
            st.connected = true;
            changed |= BTC_CHANGED_CONNECTION;
            tx_kick();
            break;
        }
        case RFCOMM_EVENT_CAN_SEND_NOW:
            tx_send();
            break;
        case RFCOMM_EVENT_CHANNEL_CLOSED:
            btc_log("spp: closed");
            cid = 0;
            st.connected = false;
            st.channel = SPP_CHANNEL;
            memset(st.peer, 0, sizeof(st.peer));
            tx_head = tx_count = 0;
            changed |= BTC_CHANGED_CONNECTION;
            break;
        default:
            break;
    }
}

int btc_spp_start(const char *name, size_t ring_bytes) {
    if (started || !btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    if (name && *name) {
        strncpy(service_name, name, sizeof(service_name) - 1);
        service_name[sizeof(service_name) - 1] = 0;
    }
    rx_cap = ring_bytes ? ring_bytes : RX_MAX;
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
    btc_rfcomm_init_once();
    if (rfcomm_register_service(on_rfcomm, SPP_CHANNEL, 0xffff) != ERROR_CODE_SUCCESS) {
        btc_btstack_free(rx);
        btc_btstack_free(tx);
        rx = tx = NULL;
        return BTC_ERR_FAIL;
    }
    memset(sdp_spp, 0, sizeof(sdp_spp));
    spp_create_sdp_record(sdp_spp, sdp_create_service_record_handle(), SPP_CHANNEL, service_name);
    sdp_register_service(sdp_spp);
    st.channel = SPP_CHANNEL;
    st.listening = true;
    started = true;
    btc_radio_profile(BTC_PROFILE_SERIAL, true, 0);
    return BTC_OK;
}

void btc_spp_stop(void) {
    if (!started) {
        return;
    }
    btc_radio_profile(BTC_PROFILE_SERIAL, false, 0);
    if (cid) {
        rfcomm_disconnect(cid);
        cid = 0;
    }
    sdp_unregister_service(big_endian_read_32(sdp_spp, 9));
    rfcomm_unregister_service(SPP_CHANNEL);
    started = false;
    st.listening = st.connected = false;
    btc_btstack_free(rx);
    btc_btstack_free(tx);
    rx = tx = NULL;
    rx_count = tx_count = 0;
}

void btc_spp_radio_stopping(void) {
    btc_spp_stop();
}

bool btc_spp_started(void) {
    return started;
}

size_t btc_spp_write_room(void) {
    return cid && tx ? TX_MAX - tx_count : 0;
}

bool btc_spp_connected(void) {
    return cid != 0;
}

size_t btc_spp_any(void) {
    return rx_count;
}

size_t btc_spp_read(uint8_t *dst, size_t size) {
    size_t n = size < rx_count ? size : rx_count;
    for (size_t i = 0; i < n; i++) {
        dst[i] = rx[rx_head];
        rx_head = (rx_head + 1) % rx_cap;
    }
    rx_count -= n;
    return n;
}

size_t btc_spp_write(const uint8_t *src, size_t size) {
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

uint32_t btc_spp_changes(void) {
    uint32_t c = changed;
    changed = 0;
    return c;
}

// Call another device's serial port: SDP finds its RFCOMM channel.
static bd_addr_t connect_addr;

static void on_sdp_query(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    static uint8_t found;
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_RFCOMM_SERVICE:
            if (!found) {
                found = sdp_event_query_rfcomm_service_get_rfcomm_channel(packet);
            }
            break;
        case SDP_EVENT_QUERY_COMPLETE:
            if (found) {
                uint16_t c;
                uint8_t s = rfcomm_create_channel(on_rfcomm, connect_addr, found, &c);
                btc_log("spp: calling %s channel %u: 0x%02x", bd_addr_to_str(connect_addr), found, s);
            } else {
                btc_log("spp: %s has no serial port", bd_addr_to_str(connect_addr));
            }
            found = 0;
            break;
        default:
            break;
    }
}

int btc_spp_connect(const uint8_t *addr, int channel) {
    if (!started || cid) {
        return BTC_ERR_STATE;
    }
    if (channel < 0 || channel > 30) {
        return BTC_ERR_ARG;
    }
    memcpy(connect_addr, addr, 6);
    if (channel) {
        uint16_t c;
        uint8_t s = rfcomm_create_channel(on_rfcomm, connect_addr, (uint8_t)channel, &c);
        btc_log("spp: calling %s channel %d: 0x%02x", bd_addr_to_str(connect_addr), channel, s);
        return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
    }
    uint8_t s = sdp_client_query_rfcomm_channel_and_name_for_service_class_uuid(on_sdp_query, connect_addr,
        BLUETOOTH_SERVICE_CLASS_SERIAL_PORT);
    return s == ERROR_CODE_SUCCESS ? BTC_OK : BTC_ERR_FAIL;
}

int btc_spp_disconnect(void) {
    if (!cid) {
        return BTC_ERR_STATE;
    }
    rfcomm_disconnect(cid);
    return BTC_OK;
}

void btc_spp_status(btc_spp_status_t *out) {
    *out = st;
}

void btc_spp_radio_lost(void) {
    started = false;
    rx = tx = NULL;
    rx_count = tx_count = 0;
    cid = 0;
    memset(&st, 0, sizeof(st));
}
