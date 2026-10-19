// SPDX-License-Identifier: MIT
//
// Generic host transfers: the C half of usb.core. A Python class driver
// claims an interface, then moves data with blocking control, bulk and
// interrupt transfers, the way PyUSB and CircuitPython's usb.core do. The
// class drivers that used to be C here (CDC, HID, MSC, MIDI) become Python
// packages over these calls. Isochronous endpoints are streams instead
// (usbif_host_stream.c), opened on interfaces claimed here.
//
// Errors come back as negative MicroPython errno values (py/mperrno.h), for
// the binding to raise as OSError.
//
// Every call blocks the caller until its transfer completes or times out.
// The completion is delivered by the host task, inside its locked pump, so
// the caller lets the host lock go while it waits.
//
// A transfer that times out is still queued in IDF's host library. A bulk or
// interrupt one is retired by halting, flushing and clearing its endpoint,
// and the wait goes on until its callback says so. A control transfer can't
// be cancelled (IDF ignores timeout_ms on EP0), so it is handed to
// usbif_host_ctrl_abandon(), which frees it whenever it does complete.

#include "py/mpconfig.h"

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usbif_ticks.h"
#include "usbif_host_stream.h"

extern usb_host_client_handle_t usbif_host_client_get(void);
extern void usbif_host_lock(void);
extern void usbif_host_unlock(void);
extern int usbif_host_lock_suspend(void);
extern void usbif_host_lock_resume(int held);
extern int usbif_host_dev_lookup(uint32_t dev_id, usb_device_handle_t *out);
extern void usbif_host_ctrl_abandon(usb_transfer_t *xfer);
extern bool usbif_host_ctrl_reclaim(usb_transfer_t *xfer);

#define USBIF_XFER_MAX_CLAIMS (16)
#define USBIF_XFER_MAX_PIPES (16)

typedef struct {
    bool in_use;
    usb_device_handle_t dev;
    uint8_t itf;
    uint8_t alt;
} usbif_xfer_claim_t;

// One cached transfer per (device, endpoint), reused call to call. A pipe
// with a transfer still in flight (timed out, not yet retired) is never
// reused or freed until its callback has run.
typedef struct {
    bool in_use;
    usb_device_handle_t dev;
    uint8_t ep;
    uint16_t mps;
    usb_transfer_t *xfer;
    SemaphoreHandle_t done;
    volatile bool busy;
    // A poll that timed out (raise_on_timeout=False) leaves its IN transfer
    // armed rather than retiring it: the next read of this endpoint collects
    // it, so polling an interrupt endpoint costs neither a halt and flush
    // each time nor a report that lands between polls.
    bool kept;
} usbif_xfer_pipe_t;

static usbif_xfer_claim_t usbif_xfer_claims[USBIF_XFER_MAX_CLAIMS];
static usbif_xfer_pipe_t usbif_xfer_pipes[USBIF_XFER_MAX_PIPES];
static SemaphoreHandle_t usbif_xfer_ctrl_done;
static volatile bool usbif_xfer_ctrl_busy;

static void usbif_xfer_cb(usb_transfer_t *xfer) {
    usbif_xfer_pipe_t *pipe = (usbif_xfer_pipe_t *)xfer->context;
    pipe->busy = false;
    xSemaphoreGive(pipe->done);
}

static void usbif_xfer_ctrl_cb(usb_transfer_t *xfer) {
    if (usbif_host_ctrl_reclaim(xfer)) {
        return;     // one we gave up on earlier; it frees itself
    }
    usbif_xfer_ctrl_busy = false;
    xSemaphoreGive(usbif_xfer_ctrl_done);
}

static int usbif_xfer_status_errno(usb_transfer_status_t status) {
    switch (status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            return 0;
        case USB_TRANSFER_STATUS_TIMED_OUT:
            return -MP_ETIMEDOUT;
        case USB_TRANSFER_STATUS_STALL:
            return -MP_EPIPE;
        case USB_TRANSFER_STATUS_NO_DEVICE:
            return -MP_ENODEV;
        default:
            return -MP_EIO;
    }
}

// Wait for a semaphore with the host lock let go, so the host task can
// deliver the completion. timeout_ms 0 waits for good.
//
// The host task paces itself by the FreeRTOS tick, 10 ms on MicroPython's
// ESP32 ports, so a transfer the bus finishes in a millisecond would wait up
// to ten for its callback; a SCSI command is three transfers. So for the
// first USBIF_XFER_SPIN_US the caller pumps the client's events itself,
// which delivers the callback as soon as the transfer is done, and only then
// falls back to sleeping by the tick.
#define USBIF_XFER_SPIN_US (20000)

static bool usbif_xfer_wait(SemaphoreHandle_t sem, uint32_t timeout_ms) {
    int held = usbif_host_lock_suspend();
    int64_t start = esp_timer_get_time();
    int64_t limit = timeout_ms ? (int64_t)timeout_ms * 1000 : INT64_MAX;
    int64_t spin = limit < USBIF_XFER_SPIN_US ? limit : USBIF_XFER_SPIN_US;
    bool got = false;
    while (!(got = xSemaphoreTake(sem, 0) == pdTRUE) && esp_timer_get_time() - start < spin) {
        usbif_host_lock();
        usb_host_client_handle_events(usbif_host_client_get(), 0);
        usbif_host_unlock();
        esp_rom_delay_us(20);
    }
    if (!got) {
        int64_t left_us = limit - (esp_timer_get_time() - start);
        if (left_us > 0) {
            TickType_t ticks = timeout_ms ? USBIF_MS_TICKS((uint32_t)(left_us / 1000)) : portMAX_DELAY;
            got = xSemaphoreTake(sem, ticks ? ticks : 1) == pdTRUE;
        }
    }
    usbif_host_lock_resume(held);
    return got;
}

// -- interfaces

static usbif_xfer_claim_t *usbif_xfer_find_claim(usb_device_handle_t dev, uint8_t itf) {
    for (int i = 0; i < USBIF_XFER_MAX_CLAIMS; i++) {
        usbif_xfer_claim_t *c = &usbif_xfer_claims[i];
        if (c->in_use && c->dev == dev && c->itf == itf) {
            return c;
        }
    }
    return NULL;
}

static void usbif_xfer_drop_pipes(usb_device_handle_t dev, int itf, bool device_gone);

// 1 for a fresh claim (the host's pipes start at DATA0), 0 when the
// interface was already claimed at that alternate, or a negative errno.
int usbif_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt) {
    usbif_host_lock();
    int r = 0;
    usb_device_handle_t dev;
    if (usbif_host_dev_lookup(dev_id, &dev) != 0) {
        r = -MP_ENODEV;
        goto out;
    }
    usbif_xfer_claim_t *c = usbif_xfer_find_claim(dev, itf);
    if (c != NULL && c->alt == alt) {
        goto out;   // 0: already ours, pipes and toggles as they were
    }
    if (c != NULL) {
        // Another alternate setting: release and claim again, which is how
        // IDF's library sends SET_INTERFACE.
        usbif_xfer_drop_pipes(dev, itf, false);
        usb_host_interface_release(usbif_host_client_get(), dev, itf);
        c->in_use = false;
    } else {
        for (int i = 0; i < USBIF_XFER_MAX_CLAIMS && c == NULL; i++) {
            if (!usbif_xfer_claims[i].in_use) {
                c = &usbif_xfer_claims[i];
            }
        }
        if (c == NULL) {
            r = -MP_ENOMEM;
            goto out;
        }
    }
    esp_err_t err = usb_host_interface_claim(usbif_host_client_get(), dev, itf, alt);
    if (err != ESP_OK) {
        r = err == ESP_ERR_NOT_FOUND ? -MP_ENOENT : -MP_EBUSY;
        goto out;
    }
    c->in_use = true;
    c->dev = dev;
    c->itf = itf;
    c->alt = alt;
    r = 1;          // a fresh claim: new pipes, at DATA0
out:
    usbif_host_unlock();
    return r;
}

// Release with the lock held. Retries while the client pump retires the
// cancellations, as the class drivers do (usbif_host_cdc.c).
static int usbif_xfer_release_locked(usbif_xfer_claim_t *c) {
    usbif_xfer_drop_pipes(c->dev, c->itf, false);
    esp_err_t err = ESP_FAIL;
    const TickType_t limit = USBIF_MS_TICKS(250);
    for (TickType_t i = 0; i < limit; i++) {
        err = usb_host_interface_release(usbif_host_client_get(), c->dev, c->itf);
        if (err == ESP_OK) {
            break;
        }
        int held = usbif_host_lock_suspend();
        vTaskDelay(1);
        usbif_host_lock_resume(held);
    }
    c->in_use = false;
    return err == ESP_OK ? 0 : -MP_EBUSY;
}

int usbif_xfer_release(uint32_t dev_id, uint8_t itf) {
    usbif_host_lock();
    int r = 0;
    usb_device_handle_t dev;
    if (usbif_host_dev_lookup(dev_id, &dev) != 0) {
        r = -MP_ENODEV;
    } else {
        usbif_xfer_claim_t *c = usbif_xfer_find_claim(dev, itf);
        r = c != NULL ? usbif_xfer_release_locked(c) : 0;
    }
    usbif_host_unlock();
    return r;
}

// -- pipes

// The endpoint's wMaxPacketSize under the claimed alternate setting of its
// interface, or under the first interface that has it when none is claimed.
// 0 when the active configuration has no such endpoint.
static uint16_t usbif_xfer_ep_mps(usb_device_handle_t dev, uint8_t ep, int *itf_out) {
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int itf = -1, alt = -1;
    uint16_t first = 0;
    int first_itf = -1;
    p += cfg->bLength;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[0] >= 9) {
            itf = p[2];
            alt = p[3];
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[0] >= 7 && p[2] == ep) {
            uint16_t mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            usbif_xfer_claim_t *c = itf >= 0 ? usbif_xfer_find_claim(dev, (uint8_t)itf) : NULL;
            if (c != NULL && c->alt == alt) {
                *itf_out = itf;
                return mps;
            }
            if (first == 0) {
                first = mps;
                first_itf = itf;
            }
        }
        p += p[0];
    }
    *itf_out = first_itf;
    return first;
}

// An isochronous endpoint of a claimed interface, for a stream: its raw
// wMaxPacketSize (mult bits included), bInterval and interface. 0, or
// -ENOENT when the configuration has no such endpoint, -EACCES when its
// interface isn't claimed at an alternate setting that carries it, -EINVAL
// when it isn't isochronous. With the host lock held.
int usbif_xfer_iso_ep(usb_device_handle_t dev, uint8_t ep, uint16_t *wmax, uint8_t *interval, uint8_t *itf_out) {
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        return -MP_ENODEV;
    }
    const uint8_t *p = (const uint8_t *)cfg;
    const uint8_t *end = p + cfg->wTotalLength;
    int itf = -1, alt = -1, r = -MP_ENOENT;
    p += cfg->bLength;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[0] >= 9) {
            itf = p[2];
            alt = p[3];
        } else if (p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && p[0] >= 7 && p[2] == ep && itf >= 0) {
            usbif_xfer_claim_t *c = usbif_xfer_find_claim(dev, (uint8_t)itf);
            if (c != NULL && c->alt == alt) {
                if ((p[3] & 0x03) != 0x01) {
                    return -MP_EINVAL;
                }
                *wmax = (uint16_t)(p[4] | (p[5] << 8));
                *interval = p[6];
                *itf_out = (uint8_t)itf;
                return 0;
            }
            r = -MP_EACCES;
        }
        p += p[0];
    }
    return r;
}

static usbif_xfer_pipe_t *usbif_xfer_pipe(usb_device_handle_t dev, uint8_t ep, size_t len, int *err) {
    usbif_xfer_pipe_t *pipe = NULL, *free_slot = NULL;
    for (int i = 0; i < USBIF_XFER_MAX_PIPES; i++) {
        usbif_xfer_pipe_t *p = &usbif_xfer_pipes[i];
        if (p->in_use && p->dev == dev && p->ep == ep) {
            pipe = p;
            break;
        }
        if (!p->in_use && free_slot == NULL) {
            free_slot = p;
        }
    }
    if (pipe != NULL && pipe->busy && !pipe->kept) {
        *err = -MP_EBUSY;      // the last one timed out and hasn't retired yet
        return NULL;
    }
    if (pipe != NULL && pipe->busy) {
        return pipe;           // a kept poll, still armed: the caller waits on it
    }
    int itf;
    uint16_t mps = usbif_xfer_ep_mps(dev, ep, &itf);
    if (mps == 0) {
        *err = -MP_ENOENT;
        return NULL;
    }
    if (usbif_xfer_find_claim(dev, (uint8_t)itf) == NULL) {
        *err = -MP_EACCES;     // claim the interface first
        return NULL;
    }
    // IN transfers must be whole packets; size the buffer up to them.
    size_t size = (len + mps - 1) / mps * mps;
    if (size == 0) {
        size = mps;
    }
    if (pipe != NULL && (pipe->mps != mps || (size_t)pipe->xfer->data_buffer_size < size)) {
        usb_host_transfer_free(pipe->xfer);
        pipe->xfer = NULL;
    }
    if (pipe == NULL) {
        if (free_slot == NULL) {
            *err = -MP_ENOMEM;
            return NULL;
        }
        pipe = free_slot;
        memset(pipe, 0, sizeof(*pipe));
        pipe->done = xSemaphoreCreateBinary();
        if (pipe->done == NULL) {
            *err = -MP_ENOMEM;
            return NULL;
        }
        pipe->in_use = true;
        pipe->dev = dev;
        pipe->ep = ep;
    }
    if (pipe->xfer == NULL) {
        if (usb_host_transfer_alloc(size, 0, &pipe->xfer) != ESP_OK) {
            *err = -MP_ENOMEM;
            return NULL;
        }
    }
    pipe->mps = mps;
    return pipe;
}

// Halt, flush and clear an endpoint whose transfer timed out, then wait for
// the callback that says the transfer is back in our hands.
static void usbif_xfer_retire(usbif_xfer_pipe_t *pipe) {
    usb_host_endpoint_halt(pipe->dev, pipe->ep);
    usb_host_endpoint_flush(pipe->dev, pipe->ep);
    usbif_xfer_wait(pipe->done, 100);
    usb_host_endpoint_clear(pipe->dev, pipe->ep);
}

// `keep`: an IN transfer that times out stays armed and this returns 0, for
// a poll that treats "nothing yet" as normal (usb.core's read with
// raise_on_timeout=False). A later call on the endpoint picks it up.
int usbif_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep) {
    usbif_host_lock();
    int r;
    usb_device_handle_t dev;
    if (usbif_host_dev_lookup(dev_id, &dev) != 0) {
        r = -MP_ENODEV;
        goto out;
    }
    usbif_xfer_pipe_t *pipe = usbif_xfer_pipe(dev, ep, len, &r);
    if (pipe == NULL) {
        goto out;
    }
    usb_transfer_t *x = pipe->xfer;
    bool in = (ep & 0x80) != 0;
    if (!pipe->busy) {
        if (!in) {
            memcpy(x->data_buffer, data, len);
        }
        x->device_handle = dev;
        x->bEndpointAddress = ep;
        x->num_bytes = in ? (int)((len + pipe->mps - 1) / pipe->mps * pipe->mps) : (int)len;
        if (in && x->num_bytes == 0) {
            x->num_bytes = pipe->mps;
        }
        x->callback = usbif_xfer_cb;
        x->context = pipe;
        x->timeout_ms = timeout_ms;
        xSemaphoreTake(pipe->done, 0);
        pipe->busy = true;
        if (usb_host_transfer_submit(x) != ESP_OK) {
            pipe->busy = false;
            r = -MP_EIO;
            goto out;
        }
    }
    pipe->kept = false;
    if (!usbif_xfer_wait(pipe->done, timeout_ms)) {
        if (keep && in) {
            pipe->kept = true;
            r = 0;
            goto out;
        }
        usbif_xfer_retire(pipe);
        // Data can land between the timeout and the halt: a MIDI note
        // arriving as a poll gives up. Hand it over rather than drop it.
        if (pipe->busy || x->actual_num_bytes <= 0 || !in) {
            r = -MP_ETIMEDOUT;
            goto out;
        }
        x->status = USB_TRANSFER_STATUS_COMPLETED;
    }
    r = usbif_xfer_status_errno(x->status);
    if (r == 0) {
        r = x->actual_num_bytes;
        if (in) {
            // A device may send more than was asked for when len isn't a
            // whole packet; the rest is dropped, as libusb reports overflow.
            memcpy(data, x->data_buffer, (size_t)r < len ? (size_t)r : len);
            if ((size_t)r > len) {
                r = (int)len;
            }
        }
    } else if (r == -MP_EPIPE) {
        usb_host_endpoint_clear(dev, ep);
    }
out:
    usbif_host_unlock();
    return r;
}

// Free the pipes of one interface (itf >= 0) or of the whole device. Pipes
// still in flight are halted and flushed first, so their callbacks arrive
// before the transfers are freed; a gone device has nothing to halt, and
// its callbacks are already queued with NO_DEVICE.
static void usbif_xfer_drop_pipes(usb_device_handle_t dev, int itf, bool device_gone) {
    // A stream on these interfaces goes first: its transfers hold the
    // endpoints the release is about to give up.
    usbif_stream_stop_itf(dev, itf, device_gone);
    for (int i = 0; i < USBIF_XFER_MAX_PIPES; i++) {
        usbif_xfer_pipe_t *p = &usbif_xfer_pipes[i];
        if (!p->in_use || p->dev != dev) {
            continue;
        }
        if (itf >= 0) {
            int ep_itf;
            usbif_xfer_ep_mps(dev, p->ep, &ep_itf);
            if (ep_itf != itf) {
                continue;
            }
        }
        if (p->busy) {
            if (!device_gone) {
                usb_host_endpoint_halt(dev, p->ep);
                usb_host_endpoint_flush(dev, p->ep);
            }
            usbif_xfer_wait(p->done, 100);
            if (!device_gone) {
                usb_host_endpoint_clear(dev, p->ep);
            }
        }
        if (p->busy) {
            continue;   // never retired: leak it rather than free it under the library
        }
        if (p->xfer != NULL) {
            usb_host_transfer_free(p->xfer);
        }
        vSemaphoreDelete(p->done);
        p->in_use = false;
    }
}

// -- control

int usbif_xfer_ctrl(uint32_t dev_id, const uint8_t setup[8], uint8_t *data, uint16_t len, uint32_t timeout_ms) {
    usbif_host_lock();
    int r;
    usb_device_handle_t dev;
    usb_transfer_t *x = NULL;
    if (usbif_host_dev_lookup(dev_id, &dev) != 0) {
        r = -MP_ENODEV;
        goto out;
    }
    if (usbif_xfer_ctrl_done == NULL) {
        usbif_xfer_ctrl_done = xSemaphoreCreateBinary();
    }
    if (usbif_xfer_ctrl_busy) {
        r = -MP_EBUSY;
        goto out;
    }
    // A fresh transfer each time: one that times out is abandoned, not
    // reused. wLength is in the setup packet.
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + len + 64, 0, &x) != ESP_OK) {
        r = -MP_ENOMEM;
        goto out;
    }
    bool in = (setup[0] & 0x80) != 0;
    memcpy(x->data_buffer, setup, 8);
    if (!in && len) {
        memcpy(x->data_buffer + 8, data, len);
    }
    x->device_handle = dev;
    x->bEndpointAddress = 0;
    x->num_bytes = 8 + len;
    x->callback = usbif_xfer_ctrl_cb;
    x->context = NULL;
    x->timeout_ms = timeout_ms;
    xSemaphoreTake(usbif_xfer_ctrl_done, 0);
    usbif_xfer_ctrl_busy = true;
    if (usb_host_transfer_submit_control(usbif_host_client_get(), x) != ESP_OK) {
        usbif_xfer_ctrl_busy = false;
        usb_host_transfer_free(x);
        r = -MP_EIO;
        goto out;
    }
    if (!usbif_xfer_wait(usbif_xfer_ctrl_done, timeout_ms)) {
        usbif_host_ctrl_abandon(x);
        usbif_xfer_ctrl_busy = false;
        r = -MP_ETIMEDOUT;
        goto out;
    }
    r = usbif_xfer_status_errno(x->status);
    if (r == 0) {
        int got = x->actual_num_bytes - 8;
        got = got < 0 ? 0 : got > len ? len : got;
        if (in) {
            memcpy(data, x->data_buffer + 8, got);
        }
        r = in ? got : len;
    }
    usb_host_transfer_free(x);
out:
    usbif_host_unlock();
    return r;
}

// -- teardown

// Called by usbif_host.c with the host lock held, as a device disappears.
void usbif_xfer_on_dev_gone(usb_device_handle_t dev) {
    usbif_xfer_drop_pipes(dev, -1, true);
    for (int i = 0; i < USBIF_XFER_MAX_CLAIMS; i++) {
        usbif_xfer_claim_t *c = &usbif_xfer_claims[i];
        if (c->in_use && c->dev == dev) {
            usb_host_interface_release(usbif_host_client_get(), dev, c->itf);
            c->in_use = false;
        }
    }
}

// Release everything, while the host task still pumps (host_stop()).
void usbif_xfer_close(void) {
    usbif_host_lock();
    for (int i = 0; i < USBIF_XFER_MAX_CLAIMS; i++) {
        if (usbif_xfer_claims[i].in_use) {
            usbif_xfer_release_locked(&usbif_xfer_claims[i]);
        }
    }
    usbif_host_unlock();
}

#endif // CONFIG_SOC_USB_OTG_SUPPORTED
