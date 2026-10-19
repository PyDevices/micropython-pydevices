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

extern int usbhost_host_dev_lookup(uint32_t dev_id, usb_device_handle_t *out);
extern int usbhost_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt);
extern int usbhost_xfer_release(uint32_t dev_id, uint8_t itf);
extern void usbhost_host_lock(void);
extern void usbhost_host_unlock(void);

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
} usbhost_net_t;

static usbhost_net_t usbhost_net;

static void usbhost_net_rx_cb(usb_transfer_t *x);

static void usbhost_net_rx_frame(const uint8_t *frame, uint32_t len) {
    if (len < 14 || len > 1536) {
        usbhost_net.rx_errors++;
        return;
    }
    uint8_t *copy = malloc(len);
    if (copy == NULL) {
        usbhost_net.rx_dropped++;
        return;
    }
    memcpy(copy, frame, len);
    // The stack frees the copy through usbhost_net_free_rx, refused or not.
    if (esp_netif_receive(usbhost_net.netif, copy, len, copy) != ESP_OK) {
        usbhost_net.rx_dropped++;
        return;
    }
    usbhost_net.rx_frames++;
}

static void usbhost_net_rx_cb(usb_transfer_t *x) {
    int slot = (int)(intptr_t)x->context;
    if (!usbhost_net.open) {
        usbhost_net.rx_live[slot] = false;
        return;
    }
    if (x->status == USB_TRANSFER_STATUS_COMPLETED && x->actual_num_bytes > 0) {
        const uint8_t *p = x->data_buffer;
        uint32_t n = (uint32_t)x->actual_num_bytes;
        if (!usbhost_net.rndis) {
            usbhost_net_rx_frame(p, n);
        } else {
            // One or more REMOTE_NDIS_PACKET_MSGs, back to back.
            while (n >= RNDIS_HEADER) {
                uint32_t type = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
                uint32_t mlen = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
                uint32_t off = p[8] | p[9] << 8 | p[10] << 16 | (uint32_t)p[11] << 24;
                uint32_t dlen = p[12] | p[13] << 8 | p[14] << 16 | (uint32_t)p[15] << 24;
                if (mlen < RNDIS_HEADER || mlen > n) {
                    usbhost_net.rx_errors++;
                    break;
                }
                if (type == RNDIS_PACKET_MSG && 8 + off + dlen <= mlen) {
                    usbhost_net_rx_frame(p + 8 + off, dlen);
                }
                p += mlen;
                n -= mlen;
            }
        }
    } else if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        usbhost_net.rx_errors++;
        if (x->status == USB_TRANSFER_STATUS_NO_DEVICE || x->status == USB_TRANSFER_STATUS_CANCELED) {
            usbhost_net.rx_live[slot] = false;
            return;
        }
    }
    x->num_bytes = NET_RX_BYTES;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        usbhost_net.rx_live[slot] = false;
    }
}

static void usbhost_net_tx_cb(usb_transfer_t *x) {
    usbhost_net.tx_busy[(int)(intptr_t)x->context] = false;
}

// From lwIP's thread: one frame out, wrapped for RNDIS.
static esp_err_t usbhost_net_transmit(void *h, void *buffer, size_t len) {
    (void)h;
    if (!usbhost_net.open || len > NET_TX_BYTES - RNDIS_HEADER - 1) {
        usbhost_net.tx_dropped++;
        return ESP_FAIL;
    }
    int slot = -1;
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (!usbhost_net.tx_busy[i]) {
            usbhost_net.tx_busy[i] = true;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        usbhost_net.tx_dropped++;     // the bus is behind: drop, as a NIC would
        return ESP_OK;
    }
    usb_transfer_t *x = usbhost_net.tx[slot];
    uint8_t *p = x->data_buffer;
    uint32_t n;
    if (usbhost_net.rndis) {
        memset(p, 0, RNDIS_HEADER);
        uint32_t mlen = RNDIS_HEADER + (uint32_t)len;
        uint32_t fields[4] = { RNDIS_PACKET_MSG, mlen, RNDIS_HEADER - 8, (uint32_t)len };
        memcpy(p, fields, sizeof(fields));      // little-endian, as RNDIS is
        memcpy(p + RNDIS_HEADER, buffer, len);
        n = mlen;
        // A transfer that ends on a whole packet would need a zero-length
        // packet after it; RNDIS allows one byte of padding instead.
        if (n % usbhost_net.mps == 0) {
            p[n++] = 0;
        }
        x->flags = 0;
    } else {
        memcpy(p, buffer, len);
        n = (uint32_t)len;
        x->flags = (n % usbhost_net.mps == 0) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    }
    x->device_handle = usbhost_net.dev;
    x->bEndpointAddress = usbhost_net.ep_out;
    x->num_bytes = (int)n;
    x->callback = usbhost_net_tx_cb;
    x->context = (void *)(intptr_t)slot;
    if (usb_host_transfer_submit(x) != ESP_OK) {
        usbhost_net.tx_busy[slot] = false;
        usbhost_net.tx_dropped++;
        return ESP_FAIL;
    }
    usbhost_net.tx_frames++;
    return ESP_OK;
}

static void usbhost_net_free_rx(void *h, void *buffer) {
    (void)h;
    free(buffer);
}

static esp_err_t usbhost_net_post_attach(esp_netif_t *netif, void *args) {
    usbhost_net_t *n = args;
    n->base.netif = netif;
    const esp_netif_driver_ifconfig_t cfg = {
        .handle = n,
        .transmit = usbhost_net_transmit,
        .driver_free_rx_buffer = usbhost_net_free_rx,
    };
    return esp_netif_set_driver_config(netif, &cfg);
}

static void usbhost_net_free_xfers(void) {
    for (int i = 0; i < NET_RX_XFERS; i++) {
        if (usbhost_net.rx[i] != NULL && !usbhost_net.rx_live[i]) {
            usb_host_transfer_free(usbhost_net.rx[i]);
            usbhost_net.rx[i] = NULL;
        }
    }
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (usbhost_net.tx[i] != NULL && !usbhost_net.tx_busy[i]) {
            usb_host_transfer_free(usbhost_net.tx[i]);
            usbhost_net.tx[i] = NULL;
        }
    }
}

// Start moving frames on the data interface `itf` (claimed here) and bring
// up a network interface with `mac` as its address, DHCP asked for.
int usbhost_net_start(uint32_t dev_id, uint8_t itf, uint8_t ep_in, uint8_t ep_out, uint16_t mps,
    const uint8_t mac[6], bool rndis) {
    if (usbhost_net.open) {
        return -MP_EBUSY;
    }
    usb_device_handle_t dev;
    if (usbhost_host_dev_lookup(dev_id, &dev) != 0) {
        return -MP_ENODEV;
    }
    int r = usbhost_xfer_claim(dev_id, itf, 0);
    if (r < 0) {
        return r;
    }
    esp_netif_init();
    esp_event_loop_create_default();    // already there is fine
    memset(&usbhost_net, 0, sizeof(usbhost_net));
    usbhost_net.base.post_attach = usbhost_net_post_attach;
    usbhost_net.dev_id = dev_id;
    usbhost_net.dev = dev;
    usbhost_net.itf = itf;
    usbhost_net.ep_in = ep_in;
    usbhost_net.ep_out = ep_out;
    usbhost_net.mps = mps ? mps : 64;
    usbhost_net.rndis = rndis;
    for (int i = 0; i < NET_RX_XFERS; i++) {
        if (usb_host_transfer_alloc(NET_RX_BYTES, 0, &usbhost_net.rx[i]) != ESP_OK) {
            goto nomem;
        }
    }
    for (int i = 0; i < NET_TX_XFERS; i++) {
        if (usb_host_transfer_alloc(NET_TX_BYTES, 0, &usbhost_net.tx[i]) != ESP_OK) {
            goto nomem;
        }
    }
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "USB_NET";
    base.if_desc = "usb";
    base.route_prio = 40;   // under Ethernet (50) and Wi-Fi (100) when they're up too
    esp_netif_config_t cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    usbhost_net.netif = esp_netif_new(&cfg);
    if (usbhost_net.netif == NULL) {
        goto nomem;
    }
    esp_netif_attach(usbhost_net.netif, &usbhost_net);
    esp_netif_set_mac(usbhost_net.netif, (uint8_t *)mac);
    usbhost_net.open = true;
    usbhost_host_lock();
    for (int i = 0; i < NET_RX_XFERS; i++) {
        usb_transfer_t *x = usbhost_net.rx[i];
        x->device_handle = dev;
        x->bEndpointAddress = ep_in;
        x->num_bytes = NET_RX_BYTES;
        x->callback = usbhost_net_rx_cb;
        x->context = (void *)(intptr_t)i;
        usbhost_net.rx_live[i] = usb_host_transfer_submit(x) == ESP_OK;
    }
    usbhost_host_unlock();
    esp_netif_action_start(usbhost_net.netif, NULL, 0, NULL);
    esp_netif_action_connected(usbhost_net.netif, NULL, 0, NULL);     // link up: DHCP starts
    return 0;
nomem:
    usbhost_net_free_xfers();
    usbhost_xfer_release(dev_id, itf);
    return -MP_ENOMEM;
}

static void usbhost_net_quiesce(bool device_gone) {
    usbhost_net.open = false;
    if (!device_gone) {
        usb_host_endpoint_halt(usbhost_net.dev, usbhost_net.ep_in);
        usb_host_endpoint_flush(usbhost_net.dev, usbhost_net.ep_in);
        usb_host_endpoint_halt(usbhost_net.dev, usbhost_net.ep_out);
        usb_host_endpoint_flush(usbhost_net.dev, usbhost_net.ep_out);
    }
}

// Take the interface down and let the device go.
void usbhost_net_stop(void) {
    if (usbhost_net.netif == NULL) {
        return;
    }
    bool was_open = usbhost_net.open;
    esp_netif_action_disconnected(usbhost_net.netif, NULL, 0, NULL);
    esp_netif_action_stop(usbhost_net.netif, NULL, 0, NULL);
    if (was_open) {
        usbhost_host_lock();
        usbhost_net_quiesce(false);
        usbhost_host_unlock();
        // The cancelled transfers come back through the host task.
        for (int i = 0; i < 50; i++) {
            bool live = false;
            for (int j = 0; j < NET_RX_XFERS; j++) {
                live |= usbhost_net.rx_live[j];
            }
            for (int j = 0; j < NET_TX_XFERS; j++) {
                live |= usbhost_net.tx_busy[j];
            }
            if (!live) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        usbhost_host_lock();
        usb_host_endpoint_clear(usbhost_net.dev, usbhost_net.ep_in);
        usb_host_endpoint_clear(usbhost_net.dev, usbhost_net.ep_out);
        usbhost_host_unlock();
        usbhost_xfer_release(usbhost_net.dev_id, usbhost_net.itf);
    }
    usbhost_net_free_xfers();
    esp_netif_destroy(usbhost_net.netif);
    usbhost_net.netif = NULL;
}

// From usbhost_host.c, with the host lock held, as a device goes.
void usbhost_net_on_dev_gone(usb_device_handle_t dev) {
    if (usbhost_net.netif != NULL && usbhost_net.open && usbhost_net.dev == dev) {
        usbhost_net_quiesce(true);
        esp_netif_action_disconnected(usbhost_net.netif, NULL, 0, NULL);
    }
}

// The addresses (ip, netmask, gateway, dns) and the counts (rx frames, tx
// frames, rx dropped, tx dropped, rx errors).
// -1 with no interface, else 1 while frames can move, 0 when not.
int usbhost_net_info(uint32_t addr[4], uint32_t counts[5]) {
    if (usbhost_net.netif == NULL) {
        return -1;
    }
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(usbhost_net.netif, &ip);
    esp_netif_dns_info_t dns = { 0 };
    esp_netif_get_dns_info(usbhost_net.netif, ESP_NETIF_DNS_MAIN, &dns);
    addr[0] = ip.ip.addr;
    addr[1] = ip.netmask.addr;
    addr[2] = ip.gw.addr;
    addr[3] = dns.ip.u_addr.ip4.addr;
    counts[0] = usbhost_net.rx_frames;
    counts[1] = usbhost_net.tx_frames;
    counts[2] = usbhost_net.rx_dropped;
    counts[3] = usbhost_net.tx_dropped;
    counts[4] = usbhost_net.rx_errors;
    return usbhost_net.open ? 1 : 0;
}

// Make this the interface sockets use when nothing more specific applies.
void usbhost_net_set_default(void) {
    if (usbhost_net.netif != NULL) {
        esp_netif_set_default_netif(usbhost_net.netif);
    }
}

#endif // CONFIG_SOC_USB_OTG_SUPPORTED
