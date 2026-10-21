// SPDX-License-Identifier: MIT
//
// The host engine's own interface: what usbhost_host.c, usbhost_host_xfer.c,
// usbhost_host_stream.c, usbhost_host_net.c and mod_usbhost.c call across
// files. One declaration of each, so a signature can't drift between the file
// that defines a function and the files that call it (CircuitPython also
// builds with -Werror=missing-prototypes).
//
// Only for a build with the engine: it needs ESP-IDF's USB host library.

#ifndef USBHOST_HOST_H
#define USBHOST_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb/usb_host.h"
#include "shared/usbhost_ringbuf.h"

// usbhost_host.c: the host task, its devices and the client lock.
int usbhost_host_start_c(void);
void usbhost_host_stop_c(void);
bool usbhost_host_is_wedged(void);
bool usbhost_host_is_running(void);
int usbhost_host_snapshot(usbhost_event_t *out, int max);
extern uint32_t usbhost_host_attaches, usbhost_host_detaches, usbhost_host_errors;
int usbhost_host_lib_counts(int *num_devices, int *num_clients);
int usbhost_host_port_cycle(void);
void usbhost_host_set_class_filter(uint16_t mask);
void usbhost_host_set_full_speed(bool full_speed);
void usbhost_host_intr_dump(void);
int usbhost_host_desc_get(uint32_t dev_id, const uint8_t **out, uint16_t *len);
int usbhost_host_dev_lookup(uint32_t dev_id, usb_device_handle_t *out);
usb_host_client_handle_t usbhost_host_client_get(void);
void usbhost_host_lock(void);
void usbhost_host_unlock(void);
void usbhost_host_lock_debug(const char *tag);
int usbhost_host_lock_suspend(void);
void usbhost_host_lock_resume(int held);
void usbhost_host_ctrl_abandon(usb_transfer_t *xfer);
bool usbhost_host_ctrl_reclaim(usb_transfer_t *xfer);

// mod_usbhost.c: the event ring's producer hook, called only from the host task.
void usbhost_host_emit(const usbhost_event_t *event);

// usbhost_host_xfer.c: claims, control, bulk and interrupt transfers.
int usbhost_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt);
int usbhost_xfer_release(uint32_t dev_id, uint8_t itf);
int usbhost_xfer_iso_ep(usb_device_handle_t dev, uint8_t ep, uint16_t *wmax, uint8_t *interval, uint8_t *itf_out);
int usbhost_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep);
int usbhost_xfer_poll(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len);
int usbhost_xfer_ctrl(uint32_t dev_id, const uint8_t setup[8], uint8_t *data, uint16_t len, uint32_t timeout_ms);
void usbhost_xfer_on_dev_gone(usb_device_handle_t dev);
void usbhost_xfer_close(void);

// usbhost_host_net.c: a tethering adapter as an ESP-IDF network interface.
// A firmware with no network (USBHOST_NO_NETWORK) leaves the file out; the
// host's teardown then has no interface to stop.
#if defined(USBHOST_NO_NETWORK)
static inline void usbhost_net_on_dev_gone(usb_device_handle_t dev) {
    (void)dev;
}
static inline void usbhost_net_stop(void) {
}
#else
int usbhost_net_start(uint32_t dev_id, uint8_t itf, uint8_t ep_in, uint8_t ep_out, uint16_t mps,
    const uint8_t mac[6], bool rndis);
void usbhost_net_stop(void);
void usbhost_net_on_dev_gone(usb_device_handle_t dev);
int usbhost_net_info(uint32_t addr[4], uint32_t counts[5]);
void usbhost_net_set_default(void);
#endif

#endif // USBHOST_HOST_H
