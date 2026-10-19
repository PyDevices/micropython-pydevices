// SPDX-License-Identifier: MIT
//
// A hosted USB network adapter as an ESP-IDF network interface: a phone's
// USB tethering (RNDIS) or a CDC-ECM adapter. Python (usb.host.net) finds
// the device and does the control side -- RNDIS's INITIALIZE, its MAC
// address, its packet filter -- then hands the data interface to this file,
// which moves Ethernet frames between its bulk endpoints and lwIP. DHCP, DNS
// and sockets come from ESP-IDF's stack as they do for Wi-Fi or Ethernet.
//
// Frames go out as they come from the stack, one bulk transfer each (RNDIS
// wraps each in its 44-byte REMOTE_NDIS_PACKET_MSG header). Received
// transfers are unpacked here: an RNDIS transfer can carry several packet
// messages one after another, and each becomes a frame for the stack.

#include "py/mpconfig.h"

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "usb/usb_host.h"
#include "py/mperrno.h"

extern int usbif_host_dev_lookup(uint32_t dev_id, usb_device_handle_t *out);
extern int usbif_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt);
extern int usbif_xfer_release(uint32_t dev_id, uint8_t itf);
extern void usbif_host_lock(void);
extern void usbif_host_unlock(void);

// The host task hands completions back once per tick (10 ms at 100 Hz), so
// each transfer turns over about 100 times a second. RX transfers are big
// enough for the device to pack several frames into one (RNDIS's
// MaxTransferSize, which usb.host.net sends as NET_RX_BYTES); TX has a slot
// per frame lwIP may have in flight between two ticks, acks included.
#define NET_RX_XFERS (6)
#define NET_TX_XFERS (16)
#define NET_RX_BYTES (8192)         // whole 512-byte packets
#define NET_TX_BYTES (2048)         // one frame and its RNDIS header
#define RNDIS_HEADER (44)
#define RNDIS_PACKET_MSG (1)

typedef struct {
    esp_netif_driver_base_t base;   // first: esp_netif hands this back as the handle
    bool open;
    bool rndis;
    uint32_t dev_id;
    usb_device_handle_t dev;
    uint8_t itf, ep_in, ep_out;
    uint16_t mps;
    usb_transfer_t *rx[NET_RX_XFERS];
    usb_transfer_t *tx[NET_TX_XFERS];
    volatile bool tx_busy[NET_TX_XFERS];
    volatile bool rx_live[NET_RX_XFERS];
    esp_netif_t *netif;
    volatile uint32_t rx_frames, tx_frames, rx_dropped, tx_dropped, rx_errors;
} usbif_net_t;

static usbif_net_t usbif_net;

static void usbif_net_rx_cb(usb_transfer_t *x);

static void usbif_net_rx_frame(const uint8_t *frame, uint32_t len) {
    if (len < 14 || len > 1536) {
        usbif_net.rx_errors++;
        return;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        usbif_net.rx_dropped++;
        return;
    }
    memcpy(copy, frame, len);
    // The stack frees the copy through usbif_net_free_rx, refused or not.
    if (esp_netif_receive(usbif_net.netif, copy, len, copy) != ESP_OK) {
        usbif_net.rx_dropped++;
        return;
    }
    usbif_net.rx_frames++;
}

static void usbif_net_rx_cb(usb_transfer_t *x) {
    int slot = (int)(intptr_t)x->context;
    if (!usbif_net.open) {
        usbif_net.rx_live[slot] = false;
        return;
    }
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes > 0) {
        const uint8_t *p = x->data_buffer;
        uint32_t n = (uint32_t)x->actual_num_bytes;
        if (!usbif_net.rndis) {
            usbif_net_rx_frame(p, n);
        } else {
            // One or more REMOTE_NDIS_PACKET_MSGs, back to back.
            while (n >= RNDIS_HEADER) {
                uint32_t type = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
                uint32_t mlen = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
                uint32_t off = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
                uint32_t dlen = p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
                if (mlen < RNDIS_HEADER || mlen > n) {
                    usbif_net.rx_errors++;
                    break;
                }
                if (type == RNDIS_PACKET_MSG && 8 + off + dlen <= mlen) {
                    usbif_net_rx_frame(p + 8 + off, dlen);
                }
                p += mlen;
                n -= mlen;
            }
        }
    } else if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        usbif_net.rx_errors++;
        if (x->status == USB_TRANSFER_STATUS_NO_DEVICE || x->status == USB_TRANSFER_STATUS_CANCELED) {
            usbif_net.rx_live[slot] = false;
            return;
        }
    }
    x->num_bytes = NET_RX_BYTES;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        usbif_net.rx_live[slot] = false;
    }
}

static void usbif_net_tx_cb(usb_transfer_t *x) {
    usbif_net.tx_busy[(int)(intptr_t)x->context] = false;
}

// From lwIP's thread: one frame out, wrapped for RNDIS.
static esp_err_t usbif_net_transmit(void *h, void *buffer, size_t len) {
    (void)h;
    if (!usbif_net.open || len > NET_TX_BYTES - RNDIS_HEADER - 1) {
        usbif_net.tx_dropped++;
        return ESP_FAIL;
    }
    int slot = -1;
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (!usbif_net.tx_busy[i]) {
            usbif_net.tx_busy[i] = true;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        usbif_net.tx_dropped++;     // the bus is behind: drop, as a NIC would
        return ESP_OK;
    }
    usb_transfer_t *x = usbif_net.tx[slot];
    uint8_t *p = x->data_buffer;
    uint32_t n;
    if (usbif_net.rndis) {
        memset(p, 0, RNDIS_HEADER);
        uint32_t mlen = RNDIS_HEADER + (uint32_t)len;
        uint32_t fields[4] = { RNDIS_PACKET_MSG, mlen, RNDIS_HEADER - 8, (uint32_t)len };
        memcpy(p, fields, sizeof(fields));      // little-endian, as RNDIS is
        memcpy(p + RNDIS_HEADER, buffer, len);
        n = mlen;
        // A transfer that ends on a whole packet would need a zero-length
        // packet after it; RNDIS allows one byte of padding instead.
        if (n % usbif_net.mps == 0) {
            p[n++] = 0;
        }
        x->flags = 0;
    } else {
        memcpy(p, buffer, len);
        n = (uint32_t)len;
        x->flags = (n % usbif_net.mps == 0) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    }
    x->device_handle = usbif_net.dev;
    x->bEndpointAddress = usbif_net.ep_out;
    x->num_bytes = (int)n;
    x->callback = usbif_net_tx_cb;
    x->context = (void *)(intptr_t)slot;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        usbif_net.tx_busy[slot] = false;
        usbif_net.tx_dropped++;
        return ESP_FAIL;
    }
    usbif_net.tx_frames++;
    return ESP_OK;
}

static void usbif_net_free_rx(void *h, void *buffer) {
    (void)h;
    free(buffer);
}

static esp_err_t usbif_net_post_attach(esp_netif_t *netif, void *args) {
    usbif_net_t *n = args;
    n->base.netif = netif;
    const esp_netif_driver_ifconfig_t cfg = {
        .handle = n,
        .transmit = usbif_net_transmit,
        .driver_free_rx_buffer = usbif_net_free_rx,
    };
    return esp_netif_set_driver_config(netif, &cfg);
}

static void usbif_net_free_xfers(void) {
    for (int i = 0; i < NET_RX_XFERS; i++) {
        if (usbif_net.rx[i] != NULL && !usbif_net.rx_live[i]) {
            usb_host_transfer_free(usbif_net.rx[i]);
            usbif_net.rx[i] = NULL;
        }
    }
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (usbif_net.tx[i] != NULL && !usbif_net.tx_busy[i]) {
            usb_host_transfer_free(usbif_net.tx[i]);
            usbif_net.tx[i] = NULL;
        }
    }
}

// Start moving frames on the data interface `itf` (claimed here) and bring
// up a network interface with `mac` as its address, DHCP asked for.
int usbif_net_start(uint32_t dev_id, uint8_t itf, uint8_t ep_in, uint8_t ep_out, uint16_t mps,
    const uint8_t mac[6], bool rndis) {
    if (usbif_net.open) {
        return -MP_EBUSY;
    }
    usb_device_handle_t dev;
    if (usbif_host_dev_lookup(dev_id, &dev) != 0) {
        return -MP_ENODEV;
    }
    int r = usbif_xfer_claim(dev_id, itf, 0);
    if (r < 0) {
        return r;
    }
    esp_netif_init();
    esp_event_loop_create_default();    // already there is fine
    memset(&usbif_net, 0, sizeof(usbif_net));
    usbif_net.base.post_attach = usbif_net_post_attach;
    usbif_net.dev_id = dev_id;
    usbif_net.dev = dev;
    usbif_net.itf = itf;
    usbif_net.ep_in = ep_in;
    usbif_net.ep_out = ep_out;
    usbif_net.mps = mps ? mps : 64;
    usbif_net.rndis = rndis;
    for (int i = 0; i < NET_RX_XFERS; i++) {
        if (usb_host_transfer_alloc(NET_RX_BYTES, 0, &usbif_net.rx[i]) != ESP_OK) {
            goto nomem;
        }
    }
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (usb_host_transfer_alloc(NET_TX_BYTES, 0, &usbif_net.tx[i]) != ESP_OK) {
            goto nomem;
        }
    }
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB_NET";
    base.if_desc = "usb";
    base.route_prio = 40;   // under Ethernet (50) and Wi-Fi (100) when they're up too
    esp_netif_config_t cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    usbif_net.netif = esp_netif_new(&cfg);
    if (usbif_net.netif == NULL) {
        goto nomem;
    }
    esp_netif_attach(usbif_net.netif, &usbif_net);
    esp_netif_set_mac(usbif_net.netif, (uint8_t *)mac);
    usbif_net.open = true;
    usbif_host_lock();
    for (int i = 0; i < NET_RX_XFERS; i++) {
        usb_transfer_t *x = usbif_net.rx[i];
        x->device_handle = dev;
        x->bEndpointAddress = ep_in;
        x->num_bytes = NET_RX_BYTES;
        x->callback = usbif_net_rx_cb;
        x->context = (void *)(intptr_t)i;
        usbif_net.rx_live[i] = usb_host_transfer_submit(x) == ESP_OK;
    }
    usbif_host_unlock();
    esp_netif_action_start(usbif_net.netif, NULL, 0, NULL);
    esp_netif_action_connected(usbif_net.netif, NULL, 0, NULL);     // link up: DHCP starts
    return 0;
nomem:
    usbif_net_free_xfers();
    usbif_xfer_release(dev_id, itf);
    return -MP_ENOMEM;
}

static void usbif_net_quiesce(bool device_gone) {
    usbif_net.open = false;
    if (!device_gone) {
        usb_host_endpoint_halt(usbif_net.dev, usbif_net.ep_in);
        usb_host_endpoint_flush(usbif_net.dev, usbif_net.ep_in);
        usb_host_endpoint_halt(usbif_net.dev, usbif_net.ep_out);
        usb_host_endpoint_flush(usbif_net.dev, usbif_net.ep_out);
    }
}

// Take the interface down and let the device go.
void usbif_net_stop(void) {
    if (usbif_net.netif == NULL) {
        return;
    }
    bool was_open = usbif_net.open;
    esp_netif_action_disconnected(usbif_net.netif, NULL, 0, NULL);
    esp_netif_action_stop(usbif_net.netif, NULL, 0, NULL);
    if (was_open) {
        usbif_host_lock();
        usbif_net_quiesce(false);
        usbif_host_unlock();
        // The cancelled transfers come back through the host task.
        for (int i = 0; i < 50; i++) {
            bool live = false;
            for (int j = 0; j < NET_RX_XFERS; j++) {
                live |= usbif_net.rx_live[j];
            }
            for (int j = 0; j < NET_TX_XFERS; j++) {
                live |= usbif_net.tx_busy[j];
            }
            if (!live) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        usbif_host_lock();
        usb_host_endpoint_clear(usbif_net.dev, usbif_net.ep_in);
        usb_host_endpoint_clear(usbif_net.dev, usbif_net.ep_out);
        usbif_host_unlock();
        usbif_xfer_release(usbif_net.dev_id, usbif_net.itf);
    }
    usbif_net_free_xfers();
    esp_netif_destroy(usbif_net.netif);
    usbif_net.netif = NULL;
}

// From usbif_host.c, with the host lock held, as a device goes.
void usbif_net_on_dev_gone(usb_device_handle_t dev) {
    if (usbif_net.netif != NULL && usbif_net.open && usbif_net.dev == dev) {
        usbif_net_quiesce(true);
        esp_netif_action_disconnected(usbif_net.netif, NULL, 0, NULL);
    }
}

// The addresses (ip, netmask, gateway, dns) and the counts (rx frames, tx
// frames, rx dropped, tx dropped, rx errors).
// -1 with no interface, else 1 while frames can move, 0 when not.
int usbif_net_info(uint32_t addr[4], uint32_t counts[5]) {
    if (usbif_net.netif == NULL) {
        return -1;
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(usbif_net.netif, &ip);
    esp_netif_dns_info_t dns = { 0 };
    esp_netif_get_dns_info(usbif_net.netif, ESP_NETIF_DNS_MAIN, &dns);
    addr[0] = ip.ip.addr;
    addr[1] = ip.netmask.addr;
    addr[2] = ip.gw.addr;
    addr[3] = dns.ip.u_addr.ip4.addr;
    counts[0] = usbif_net.rx_frames;
    counts[1] = usbif_net.tx_frames;
    counts[2] = usbif_net.rx_dropped;
    counts[3] = usbif_net.tx_dropped;
    counts[4] = usbif_net.rx_errors;
    return usbif_net.open ? 1 : 0;
}

// Make this the interface sockets use when nothing more specific applies.
void usbif_net_set_default(void) {
    if (usbif_net.netif != NULL) {
        esp_netif_set_default_netif(usbif_net.netif);
    }
}

#endif // CONFIG_SOC_USB_OTG_SUPPORTED
