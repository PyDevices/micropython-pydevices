// SPDX-License-Identifier: MIT
//
// ESP-IDF's host engine, file to file: what usbhost_host.c,
// usbhost_host_xfer.c, usbhost_host_stream.c and usbhost_host_net.c call
// across files, beyond what the binding calls (usbhost_engine.h). One
// declaration of each, so a signature can't drift between the file that
// defines a function and the files that call it (CircuitPython also builds
// with -Werror=missing-prototypes).
//
// Only for a build with this engine: it needs ESP-IDF's USB host library.

#ifndef USBHOST_HOST_H
#define USBHOST_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb/usb_host.h"
#include "usbhost_engine.h"

// usbhost_host.c: the host task's devices and the client lock.
int usbhost_host_dev_lookup(uint32_t dev_id, usb_device_handle_t *out);
usb_host_client_handle_t usbhost_host_client_get(void);
// The largest IN and periodic OUT packet the installed FIFO split takes, in
// bytes; 0 for IN when the board's Kconfig bias chose the split.
void usbhost_host_fifo_limits(uint32_t *in_bytes, uint32_t *ptx_bytes);
void usbhost_host_lock(void);
void usbhost_host_unlock(void);
void usbhost_host_lock_debug(const char *tag);
int usbhost_host_lock_suspend(void);
void usbhost_host_lock_resume(int held);
void usbhost_host_ctrl_abandon(usb_transfer_t *xfer);
bool usbhost_host_ctrl_reclaim(usb_transfer_t *xfer);

// usbhost_host_xfer.c: what the host core and the streams need of it.
int usbhost_xfer_iso_ep(usb_device_handle_t dev, uint8_t ep, uint16_t *wmax, uint8_t *interval, uint8_t *itf_out);
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
void usbhost_net_on_dev_gone(usb_device_handle_t dev);
#endif

#endif // USBHOST_HOST_H
