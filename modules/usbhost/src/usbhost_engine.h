// SPDX-License-Identifier: MIT
//
// Which host engine a build has, and what the binding (mod_usbhost.c) calls on
// it. Nothing here names an engine's own types, so the binding compiles the
// same over any of them; each engine keeps its own types in its own header
// (usbhost_host.h for ESP-IDF's USB host library).
//
// What a build gets:
//   USBHOST_HAVE_HOST    devices, descriptors, claims, control, bulk and
//                        interrupt transfers: what usb.core needs.
//   USBHOST_HAVE_STREAM  isochronous streams (usb.host.audio, usb.host.video).
//   USBHOST_HAVE_NET     a tethering adapter as a network interface
//                        (usb.host.net).
// Without one of them the module still imports, and the calls behind it raise
// OSError, which the packages turn into their own "no backend" errors.

#ifndef USBHOST_ENGINE_H
#define USBHOST_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "py/mpconfig.h"
#include "shared/usbhost_ringbuf.h"

// ESP-IDF's USB host library, on chips with an OTG controller. Usermod
// sources build with MicroPython's define set, not the IDF's, so
// ESP_PLATFORM is NOT defined here -- learned the hard way when this gate
// silently compiled the engine out and the stubs answered in its place.
// sdkconfig.h existing (and naming an OTG controller) is the real signal that
// the IDF host library is available. A make build that leaves the engine out
// says so with USBHOST_NO_HOST_ENGINE (micropython.mk): a CircuitPython
// ESP32-S3 has the controller, and it's CircuitPython's.
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif
#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED && !defined(USBHOST_NO_HOST_ENGINE)
#define USBHOST_ENGINE_IDF (1)
#else
#define USBHOST_ENGINE_IDF (0)
#endif

#define USBHOST_HAVE_HOST (USBHOST_ENGINE_IDF)
#define USBHOST_HAVE_STREAM (USBHOST_ENGINE_IDF)

// The tethering adapter needs lwIP, which a firmware with no network leaves
// out (CircuitPython's ESP32-P4 port, until it drives the boards' ESP32-C6).
#if USBHOST_ENGINE_IDF && !defined(USBHOST_NO_NETWORK)
#define USBHOST_HAVE_NET (1)
#else
#define USBHOST_HAVE_NET (0)
#endif

// mod_usbhost.c: the event ring's producer hook. Each engine calls it from
// one context only (its host task), keeping the ring single-producer.
void usbhost_host_emit(const usbhost_event_t *event);

#if USBHOST_HAVE_HOST

// The host: start and stop, its devices, and its counters.
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

// Transfers. Each returns a negative MicroPython errno on failure.
int usbhost_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt);
int usbhost_xfer_release(uint32_t dev_id, uint8_t itf);
int usbhost_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep);
int usbhost_xfer_poll(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len);
int usbhost_xfer_ctrl(uint32_t dev_id, const uint8_t setup[8], uint8_t *data, uint16_t len, uint32_t timeout_ms);

#endif // USBHOST_HAVE_HOST

#if USBHOST_HAVE_NET
int usbhost_net_start(uint32_t dev_id, uint8_t itf, uint8_t ep_in, uint8_t ep_out, uint16_t mps,
    const uint8_t mac[6], bool rndis);
void usbhost_net_stop(void);
int usbhost_net_info(uint32_t addr[4], uint32_t counts[5]);
void usbhost_net_set_default(void);
#endif

#endif // USBHOST_ENGINE_H
