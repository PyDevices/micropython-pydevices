// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The tether on BTstack: the board as a PAN user (PANU) on a phone's network
// access point, over BNEP, with the phone's frames on an lwIP interface of
// their own beside the Wi-Fi's. See btclassic.h.
//
// Threads: BTstack runs on the interpreter's thread, but lwIP also runs from
// PendSV (its timers, the Wi-Fi driver). Frames from the phone go into lwIP
// under the lwIP lock. Frames lwIP sends wait in a short queue, taken under
// the same lock; a BTstack data source sees the queue and asks BNEP for room.

#include <string.h>

#include "py/mphal.h"
#include "extmod/modnetwork.h"

#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "netif/ethernet.h"

#include "btstack.h"
#include "btstack_run_loop_base.h"

#include "btc_btstack.h"

#if MICROPY_PY_LWIP

#define QUEUE_LEN 8
#define FRAME_MAX (14 + 1500)

static struct {
    bool started;
    bool connecting;
    bool connected;
    bool netif_added;
    bool handler_up;
    bool asked;                 // waiting for BNEP's can-send-now
    uint16_t cid;
    btc_addr_t peer;
    int result;
    uint32_t changes;
    uint8_t prev_default;       // the default interface's index before ours took over
    uint32_t rx_frames, tx_frames, tx_dropped;

    struct netif netif;
    struct pbuf *queue[QUEUE_LEN];
    uint8_t q_head, q_count;

    btstack_data_source_t source;
    btstack_packet_callback_registration_t hci_cb;
    uint8_t out[FRAME_MAX];
} t;

// ---- the queue, between lwIP and BTstack ----

// Drop what's queued. Called with the lwIP lock held.
static void queue_clear(void) {
    while (t.q_count) {
        pbuf_free(t.queue[t.q_head]);
        t.q_head = (t.q_head + 1) % QUEUE_LEN;
        t.q_count--;
    }
}

// lwIP's context: the interpreter's thread under the lwIP lock, or PendSV.
static err_t link_output(struct netif *netif, struct pbuf *p) {
    (void)netif;
    if (!t.cid || t.q_count == QUEUE_LEN) {
        t.tx_dropped++;
        return ERR_OK;          // dropped, as a busy Ethernet would; TCP sends again
    }
    pbuf_ref(p);
    t.queue[(t.q_head + t.q_count) % QUEUE_LEN] = p;
    t.q_count++;
    btstack_run_loop_poll_data_sources_from_irq();
    return ERR_OK;
}

// BTstack's run loop: ask for room when a frame waits.
static void on_poll(btstack_data_source_t *ds, btstack_data_source_callback_type_t type) {
    (void)ds;
    (void)type;
    if (t.cid && t.q_count && !t.asked) {
        t.asked = true;
        bnep_request_can_send_now_event(t.cid);
    }
}

static void send_one(void) {
    t.asked = false;
    MICROPY_PY_LWIP_ENTER
    if (!t.q_count) {
        MICROPY_PY_LWIP_EXIT
        return;
    }
    struct pbuf *p = t.queue[t.q_head];
    t.q_head = (t.q_head + 1) % QUEUE_LEN;
    t.q_count--;
    uint16_t len = pbuf_copy_partial(p, t.out, sizeof(t.out), 0);
    pbuf_free(p);
    bool more = t.q_count != 0;
    MICROPY_PY_LWIP_EXIT
    if (t.cid && bnep_send(t.cid, t.out, len) == 0) {
        t.tx_frames++;
    } else {
        t.tx_dropped++;
    }
    if (more && t.cid) {
        t.asked = true;
        bnep_request_can_send_now_event(t.cid);
    }
}

static void receive(const uint8_t *frame, uint16_t size) {
    MICROPY_PY_LWIP_ENTER
    struct pbuf *p = pbuf_alloc(PBUF_RAW, size, PBUF_POOL);
    if (p) {
        pbuf_take(p, frame, size);
        if (t.netif.input(p, &t.netif) != ERR_OK) {
            pbuf_free(p);
        } else {
            t.rx_frames++;
        }
    }
    MICROPY_PY_LWIP_EXIT
}

// ---- the interface ----

static err_t netif_init_cb(struct netif *netif) {
    netif->name[0] = 'b';
    netif->name[1] = 't';
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    netif->output = etharp_output;
    netif->linkoutput = link_output;
    #if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
    netif->flags |= NETIF_FLAG_MLD6;
    #endif
    return ERR_OK;
}

// Added once and kept: lwIP isn't started again on a soft reset, and an
// interface must only be added once.
static void netif_once(void) {
    if (t.netif_added) {
        return;
    }
    ip4_addr_t none;
    ip4_addr_set_zero(&none);
    MICROPY_PY_LWIP_ENTER
    t.netif.hwaddr_len = 6;
    netif_add(&t.netif, &none, &none, &none, NULL, netif_init_cb, ethernet_input);
    #if LWIP_NETIF_HOSTNAME
    netif_set_hostname(&t.netif, mod_network_hostname_data);
    #endif
    MICROPY_PY_LWIP_EXIT
    t.netif_added = true;
}

// The link came up: the board's Bluetooth address is its MAC, as BNEP wants,
// DHCP asks the phone for an address, and ours becomes the default route.
static void link_up(void) {
    MICROPY_PY_LWIP_ENTER
    gap_local_bd_addr(t.netif.hwaddr);
    netif_set_up(&t.netif);
    netif_set_link_up(&t.netif);
    #if LWIP_IPV6
    netif_create_ip6_linklocal_address(&t.netif, 1);
    #endif
    t.prev_default = netif_default ? netif_get_index(netif_default) : 0;
    netif_set_default(&t.netif);
    dhcp_start(&t.netif);
    MICROPY_PY_LWIP_EXIT
}

// The link went: give the default route back if the old interface is still there.
static void link_down(void) {
    if (!t.netif_added) {
        return;
    }
    MICROPY_PY_LWIP_ENTER
    queue_clear();
    dhcp_release_and_stop(&t.netif);
    netif_set_link_down(&t.netif);
    netif_set_down(&t.netif);
    if (netif_default == &t.netif) {
        struct netif *prev = t.prev_default ? netif_get_by_index(t.prev_default) : NULL;
        netif_set_default(prev != &t.netif ? prev : NULL);
    }
    MICROPY_PY_LWIP_EXIT
}

static void closed(int result) {
    bool was = t.connected || t.connecting;
    t.cid = 0;
    t.asked = false;
    t.connected = t.connecting = false;
    t.result = result;
    link_down();
    if (was) {
        t.changes |= BTC_CHANGED_TETHER;
    }
}

// ---- BTstack ----

static void on_bnep(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type == BNEP_DATA_PACKET) {
        if (t.connected) {
            receive(packet, size);
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    switch (hci_event_packet_get_type(packet)) {
        case BNEP_EVENT_CHANNEL_OPENED: {
            uint8_t status = bnep_event_channel_opened_get_status(packet);
            if (status) {
                btc_log("tether: open failed 0x%02x", status);
                closed(BTC_ERR_FAIL);
                return;
            }
            t.cid = bnep_event_channel_opened_get_bnep_cid(packet);
            bnep_event_channel_opened_get_remote_address(packet, t.peer);
            t.connecting = false;
            t.connected = true;
            t.result = BTC_OK;
            link_up();
            t.changes |= BTC_CHANGED_TETHER;
            btc_log("tether: open, mtu %u", bnep_event_channel_opened_get_mtu(packet));
            return;
        }
        case BNEP_EVENT_CHANNEL_TIMEOUT:
            btc_log("tether: setup timed out");
            closed(BTC_ERR_FAIL);
            return;
        case BNEP_EVENT_CHANNEL_CLOSED:
            btc_log("tether: closed");
            closed(BTC_OK);
            return;
        case BNEP_EVENT_CAN_SEND_NOW:
            send_one();
            return;
        default:
            return;
    }
}

// The stack is going down (a soft reset takes it with it): take the
// interface down too, so lwIP stops sending into it.
static void on_hci(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type == HCI_EVENT_PACKET && hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE
        && btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
        closed(BTC_ERR_STATE);
    }
}

// ---- btclassic.h ----

int btc_tether_start(void) {
    if (t.started) {
        return BTC_OK;
    }
    if (!btc_radio_active()) {
        return BTC_ERR_STATE;
    }
    netif_once();
    bnep_init();
    btstack_run_loop_set_data_source_handler(&t.source, &on_poll);
    btstack_run_loop_enable_data_source_callbacks(&t.source, DATA_SOURCE_CALLBACK_POLL);
    btstack_run_loop_add_data_source(&t.source);
    t.hci_cb.callback = &on_hci;
    hci_add_event_handler(&t.hci_cb);
    t.handler_up = true;
    t.started = true;
    t.changes = 0;
    t.result = BTC_OK;
    return BTC_OK;
}

void btc_tether_stop(void) {
    if (!t.started) {
        return;
    }
    if (t.connected || t.connecting) {
        bnep_disconnect(t.peer);
    }
    closed(BTC_OK);
    btstack_run_loop_remove_data_source(&t.source);
    if (t.handler_up) {
        hci_remove_event_handler(&t.hci_cb);
        t.handler_up = false;
    }
    t.started = false;
}

bool btc_tether_started(void) {
    return t.started;
}

int btc_tether_connect(const uint8_t *addr) {
    if (!t.started) {
        return BTC_ERR_STATE;
    }
    if (t.connected || t.connecting) {
        return BTC_OK;
    }
    if (addr) {
        memcpy(t.peer, addr, sizeof(t.peer));
    } else if (!btc_radio_last_peer(t.peer)) {
        btc_addr_t paired[1];
        if (btc_radio_paired(paired, 1) < 1) {
            return BTC_ERR_STATE;
        }
        memcpy(t.peer, paired[0], sizeof(t.peer));
    }
    if (bnep_connect(&on_bnep, t.peer, BLUETOOTH_PSM_BNEP, BLUETOOTH_SERVICE_CLASS_PANU, BLUETOOTH_SERVICE_CLASS_NAP)) {
        return BTC_ERR_FAIL;
    }
    t.connecting = true;
    t.result = BTC_OK;
    return BTC_OK;
}

int btc_tether_disconnect(void) {
    if (!t.started) {
        return BTC_ERR_STATE;
    }
    if (t.connected || t.connecting) {
        bnep_disconnect(t.peer);
        closed(BTC_OK);
    }
    return BTC_OK;
}

void btc_tether_status(btc_tether_status_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = t.started;
    out->connecting = t.connecting;
    out->connected = t.connected;
    memcpy(out->peer, t.peer, sizeof(out->peer));
    out->result = t.result;
    out->rx_frames = t.rx_frames;
    out->tx_frames = t.tx_frames;
    out->tx_dropped = t.tx_dropped;
}

uint32_t btc_tether_changes(void) {
    uint32_t c = t.changes;
    t.changes = 0;
    return c;
}

void *btc_tether_netif(void) {
    return t.netif_added ? &t.netif : NULL;
}

// ---- the radio's hooks ----

void btc_tether_radio_stopping(void) {
    btc_tether_stop();
}

// The stack went down underneath: forget it without calling it.
void btc_tether_radio_lost(void) {
    t.cid = 0;
    t.asked = false;
    t.connected = t.connecting = false;
    link_down();
    // The run loop keeps its list of sources across a restart: take ours out
    // without the run loop, which may be gone.
    btstack_run_loop_base_remove_data_source(&t.source);
    t.handler_up = false;
    t.started = false;
}

#else

int btc_tether_start(void) {
    return BTC_ERR_UNSUPPORTED;
}
void btc_tether_stop(void) {
}
bool btc_tether_started(void) {
    return false;
}
int btc_tether_connect(const uint8_t *addr) {
    return BTC_ERR_UNSUPPORTED;
}
int btc_tether_disconnect(void) {
    return BTC_ERR_UNSUPPORTED;
}
void btc_tether_status(btc_tether_status_t *out) {
    memset(out, 0, sizeof(*out));
}
uint32_t btc_tether_changes(void) {
    return 0;
}
void *btc_tether_netif(void) {
    return NULL;
}
void btc_tether_radio_stopping(void) {
}
void btc_tether_radio_lost(void) {
}

#endif // MICROPY_PY_LWIP
