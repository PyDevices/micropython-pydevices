// SPDX-License-Identifier: MIT
//
// The host engine on TinyUSB's host stack (USBHOST_ENGINE_TUH in
// usbhost_engine.h), for a board whose USB device already runs on TinyUSB
// and whose host port is a second controller: the device keeps one root
// port and the host takes the other. The Arduino GIGA R1 WiFi is the first:
// its USB-C port is the device (OTG_FS, root port 0) and its USB-A port the
// host (OTG_HS on its internal full-speed PHY, root port 1), with the
// socket's 5 V switched by a GPIO.
//
// The board names the root port (USBHOST_TUH_RHPORT) and the 5 V switch
// (USBHOST_TUH_VBUS_PIN, high for on), turns TinyUSB's host stack on
// (CFG_TUH_*), and gives the host task a PendSV slot
// (PENDSV_DISPATCH_USBHOST). The root port has no mode in CFG_TUSB_RHPORTx_MODE,
// so boot starts only the device; host_start() starts the host.
//
// Three contexts touch the stack. The controller's interrupt queues
// TinyUSB's events. TinyUSB's host task runs at PendSV level: the interrupt
// asks for it (tuh_event_hook_cb), and a 1 ms soft timer runs it for the
// delays TinyUSB keeps itself (debounce, port reset), which no interrupt
// announces. The interpreter claims, opens and submits. TinyUSB without an
// RTOS isn't reentrant, so the interpreter holds PendSV off around every
// call it makes into the stack, and the task and the interpreter are never
// inside it at once. A completion lands in the task, which writes the
// pipe's result and then its state; the interpreter waits on the state.
//
// Device ids are TinyUSB's device addresses. TinyUSB runs only its hub
// driver: every other interface is the Python packages', over usb.core. There
// are no isochronous streams and no network adapter here (usbhost_engine.h).

#include "py/mpconfig.h"
#include "usbhost_engine.h"

#if USBHOST_ENGINE_TUH

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/softtimer.h"
#include "pendsv.h"
#include "tusb.h"
#include "host/hcd.h"
#include "usbhost_classes.h"

#if defined(STM32H7)
#include "irq.h"
#endif

#define USBHOST_TUH_DEVS (CFG_TUH_DEVICE_MAX)
#define USBHOST_TUH_DESC_MAX (CFG_TUH_ENUMERATION_BUFSIZE)
#define USBHOST_TUH_CLAIMS (16)
#define USBHOST_TUH_PIPES (16)

// A pipe's state, and the control transfer's.
#define USBHOST_TUH_IDLE (0)
#define USBHOST_TUH_BUSY (1)
#define USBHOST_TUH_DONE (2)

typedef struct {
    uint8_t daddr;          // 0: free
    bool in_use;            // mounted, and an ATTACH sent
    uint8_t speed;          // usbhost's: 1 low, 2 full, 3 high
    uint16_t vid;
    uint16_t pid;
    uint16_t classes;
    uint16_t desc_len;
    // The configuration descriptor, kept from enumeration: TinyUSB doesn't
    // keep it, and usb.core reads it for every endpoint.
    uint8_t desc[USBHOST_TUH_DESC_MAX];
} usbhost_tuh_dev_t;

typedef struct {
    bool in_use;
    uint8_t daddr;
    uint8_t itf;
    uint8_t alt;
} usbhost_tuh_claim_t;

// One per (device, endpoint), reused call to call. Its buffer is in the
// Python heap, in usbhost_tuh_bufs[] under the pipe's index, so the
// controller never writes into a buffer the caller passed and then dropped.
typedef struct {
    bool in_use;
    uint8_t daddr;
    uint8_t ep;
    uint8_t itf;
    uint16_t mps;
    uint16_t gen;           // a completion for an older submit is dropped
    uint32_t size;          // the buffer's
    volatile uint8_t state;
    volatile uint8_t result;
    volatile uint32_t actual;
    // A read that timed out with keep stays armed: the next read collects it.
    bool kept;
    // The interpreter is waiting on it; a device that goes meanwhile marks it
    // dead and leaves the freeing to the waiter.
    bool waiter;
    volatile bool dead;
} usbhost_tuh_pipe_t;

// The pipes' buffers, then the control transfer's.
MP_REGISTER_ROOT_POINTER(void *usbhost_tuh_bufs[17]);
#define USBHOST_TUH_CTRL_BUF (USBHOST_TUH_PIPES)

static usbhost_tuh_dev_t usbhost_tuh_devs[USBHOST_TUH_DEVS];
static usbhost_tuh_claim_t usbhost_tuh_claims[USBHOST_TUH_CLAIMS];
static usbhost_tuh_pipe_t usbhost_tuh_pipes[USBHOST_TUH_PIPES];

static struct {
    uint8_t daddr;
    uint16_t gen;
    uint32_t size;
    volatile uint8_t state;
    volatile uint8_t result;
    volatile uint32_t actual;
    // A timed-out transfer TinyUSB couldn't abort (still queued behind
    // another device's): it owns the buffer until its completion arrives.
    volatile bool stale;
    tusb_control_request_t request;
} usbhost_tuh_ctrl;

static bool usbhost_tuh_running;
static uint16_t usbhost_tuh_class_filter = 0xFFFF;
static soft_timer_entry_t usbhost_tuh_timer;

uint32_t usbhost_host_attaches, usbhost_host_detaches, usbhost_host_errors;

// -- locking and the task

static uint32_t usbhost_tuh_lock(void) {
    MICROPY_PY_PENDSV_ENTER;
    return atomic_state;
}

static void usbhost_tuh_unlock(uint32_t atomic_state) {
    MICROPY_PY_PENDSV_EXIT;
}

// At PendSV level only, from the dispatch slot or the soft timer.
static void usbhost_tuh_task(void) {
    if (!usbhost_tuh_running) {
        return;
    }
    tuh_task_ext(0, false);
    if (tuh_task_event_ready()) {
        pendsv_schedule_dispatch(PENDSV_DISPATCH_USBHOST, usbhost_tuh_task);
    }
}

static void usbhost_tuh_tick(soft_timer_entry_t *self) {
    (void)self;
    usbhost_tuh_task();
}

// From the controller's interrupt, for every event it queues.
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)eventid;
    (void)in_isr;
    pendsv_schedule_dispatch(PENDSV_DISPATCH_USBHOST, usbhost_tuh_task);
}

// TinyUSB's clock for its own delays; with no RTOS the application gives it.
uint32_t tusb_time_millis_api(void) {
    return mp_hal_ticks_ms();
}

// Wait for a state to reach DONE, for timeout_ms (0: for good). The
// interpreter sleeps between interrupts and runs nothing else meanwhile, as
// it would blocked in any other call.
static bool usbhost_tuh_wait(volatile uint8_t *state, uint32_t timeout_ms) {
    uint32_t start = mp_hal_ticks_ms();
    while (*state != USBHOST_TUH_DONE) {
        if (timeout_ms && mp_hal_ticks_ms() - start >= timeout_ms) {
            return false;
        }
        __WFI();
    }
    return true;
}

// -- the controller

#if defined(STM32H7) && USBHOST_TUH_RHPORT == 1

// OTG_HS on its internal full-speed PHY: PB14 and PB15 at AF12, no ULPI.
static void usbhost_tuh_hw_on(void) {
    __HAL_RCC_USB1_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USB1_OTG_HS_ULPI_CLK_DISABLE();
    HAL_PWREx_EnableUSBVoltageDetector();
    mp_hal_pin_config(pin_B14, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, GPIO_AF12_OTG1_FS);
    mp_hal_pin_config_speed(pin_B14, MP_HAL_PIN_SPEED_VERY_HIGH);
    mp_hal_pin_config(pin_B15, MP_HAL_PIN_MODE_ALT, MP_HAL_PIN_PULL_NONE, GPIO_AF12_OTG1_FS);
    mp_hal_pin_config_speed(pin_B15, MP_HAL_PIN_SPEED_VERY_HIGH);
    NVIC_SetPriority(OTG_HS_IRQn, IRQ_PRI_OTG_HS);
}

static void usbhost_tuh_hw_off(void) {
    NVIC_DisableIRQ(OTG_HS_IRQn);
    __HAL_RCC_USB1_OTG_HS_FORCE_RESET();
    __HAL_RCC_USB1_OTG_HS_RELEASE_RESET();
    __HAL_RCC_USB1_OTG_HS_CLK_DISABLE();
    mp_hal_pin_config(pin_B14, MP_HAL_PIN_MODE_ANALOG, MP_HAL_PIN_PULL_NONE, 0);
    mp_hal_pin_config(pin_B15, MP_HAL_PIN_MODE_ANALOG, MP_HAL_PIN_PULL_NONE, 0);
}

// The port's own handler is for a device on OTG_HS (MICROPY_HW_USB_HS),
// which a board with the host there doesn't set.
void OTG_HS_IRQHandler(void) {
    IRQ_ENTER(OTG_HS_IRQn);
    tuh_int_handler(USBHOST_TUH_RHPORT, true);
    IRQ_EXIT(OTG_HS_IRQn);
}

#else
#error "usbhost: no TinyUSB host controller set-up for this MCU and root port"
#endif

static void usbhost_tuh_vbus(bool on) {
    mp_hal_pin_output(USBHOST_TUH_VBUS_PIN);
    mp_hal_pin_write(USBHOST_TUH_VBUS_PIN, on);
}

// -- devices

// With reserve, a slot that was never mounted counts as free: TinyUSB
// enumerates one device at a time, so one whose enumeration failed after
// its configuration was read (a device browning out, say) is done with it.
static usbhost_tuh_dev_t *usbhost_tuh_dev(uint8_t daddr, bool reserve) {
    usbhost_tuh_dev_t *free_slot = NULL, *stale = NULL;
    for (int i = 0; i < USBHOST_TUH_DEVS; i++) {
        usbhost_tuh_dev_t *d = &usbhost_tuh_devs[i];
        if (d->daddr == daddr) {
            return d;
        }
        if (d->daddr == 0 && free_slot == NULL) {
            free_slot = d;
        } else if (!d->in_use && stale == NULL) {
            stale = d;
        }
    }
    if (free_slot == NULL) {
        free_slot = stale;
    }
    if (reserve && free_slot != NULL) {
        memset(free_slot, 0, offsetof(usbhost_tuh_dev_t, desc));
        free_slot->daddr = daddr;
        return free_slot;
    }
    return NULL;
}

static usbhost_tuh_dev_t *usbhost_tuh_dev_mounted(uint32_t dev_id) {
    if (dev_id == 0 || dev_id > 255) {
        return NULL;
    }
    usbhost_tuh_dev_t *d = usbhost_tuh_dev((uint8_t)dev_id, false);
    return d != NULL && d->in_use ? d : NULL;
}

// Every interface's class, over the whole configuration.
static uint16_t usbhost_tuh_class_bits(const uint8_t *p, uint16_t len) {
    const uint8_t *end = p + len;
    uint16_t bits = 0;
    p += p[0];
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == TUSB_DESC_INTERFACE && p[0] >= 9) {
            switch (p[5]) {
                case TUSB_CLASS_HID:
                    bits |= USBHOST_CLASS_HID;
                    break;
                case TUSB_CLASS_MSC:
                    bits |= USBHOST_CLASS_MSC;
                    break;
                case TUSB_CLASS_CDC:
                case TUSB_CLASS_CDC_DATA:
                    bits |= USBHOST_CLASS_CDC;
                    break;
                case TUSB_CLASS_AUDIO:
                    // Subclass 3 is MIDIStreaming.
                    bits |= p[6] == 3 ? USBHOST_CLASS_MIDI : USBHOST_CLASS_UAC;
                    break;
                case TUSB_CLASS_VIDEO:
                    bits |= USBHOST_CLASS_UVC;
                    break;
                case TUSB_CLASS_HUB:
                    break;
                default:
                    bits |= USBHOST_CLASS_OTHER;
                    break;
            }
        }
        p += p[0];
    }
    return bits;
}

// Enumeration, in the task: keep the first configuration's descriptor.
bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t cfg_index, const tusb_desc_configuration_t *desc) {
    (void)cfg_index;
    usbhost_tuh_dev_t *d = usbhost_tuh_dev(daddr, true);
    if (d != NULL) {
        uint16_t len = tu_le16toh(desc->wTotalLength);
        d->desc_len = len < USBHOST_TUH_DESC_MAX ? len : USBHOST_TUH_DESC_MAX;
        memcpy(d->desc, desc, d->desc_len);
    }
    return true;
}

static void usbhost_tuh_emit(uint8_t kind, const usbhost_tuh_dev_t *d) {
    usbhost_event_t event = {
        .kind = kind,
        .speed = d->speed,
        .vid = d->vid,
        .pid = d->pid,
        .classes = d->classes,
        .dev_id = d->daddr,
    };
    usbhost_host_emit(&event);
}

void tuh_mount_cb(uint8_t daddr) {
    usbhost_tuh_dev_t *d = usbhost_tuh_dev(daddr, false);
    if (d == NULL || d->desc_len < 9) {
        usbhost_host_errors++;
        return;
    }
    uint16_t classes = usbhost_tuh_class_bits(d->desc, d->desc_len);
    if ((classes & usbhost_tuh_class_filter) == 0) {
        // A hub, or nothing host_start() asked for: invisible, as on ESP-IDF.
        d->daddr = 0;
        return;
    }
    tuh_vid_pid_get(daddr, &d->vid, &d->pid);
    switch (tuh_speed_get(daddr)) {
        case TUSB_SPEED_LOW:
            d->speed = 1;
            break;
        case TUSB_SPEED_HIGH:
            d->speed = 3;
            break;
        default:
            d->speed = 2;
            break;
    }
    d->classes = classes;
    d->in_use = true;
    usbhost_tuh_emit(USBHOST_EV_ATTACH, d);
    usbhost_host_attaches++;
}

static void usbhost_tuh_pipe_free(usbhost_tuh_pipe_t *p) {
    p->in_use = false;
    p->gen++;
    p->state = USBHOST_TUH_IDLE;
    p->kept = false;
    p->dead = false;
    p->size = 0;
    MP_STATE_VM(usbhost_tuh_bufs)[p - usbhost_tuh_pipes] = NULL;
}

// In the task, or with the lock held in the interpreter (host_stop).
void tuh_umount_cb(uint8_t daddr) {
    for (int i = 0; i < USBHOST_TUH_PIPES; i++) {
        usbhost_tuh_pipe_t *p = &usbhost_tuh_pipes[i];
        if (p->in_use && p->daddr == daddr) {
            if (p->waiter) {
                p->dead = true;
                p->state = USBHOST_TUH_DONE;
            } else {
                usbhost_tuh_pipe_free(p);
            }
        }
    }
    for (int i = 0; i < USBHOST_TUH_CLAIMS; i++) {
        if (usbhost_tuh_claims[i].daddr == daddr) {
            usbhost_tuh_claims[i].in_use = false;
            usbhost_tuh_claims[i].daddr = 0;
        }
    }
    usbhost_tuh_dev_t *d = usbhost_tuh_dev(daddr, false);
    if (d == NULL) {
        return;
    }
    if (d->in_use) {
        usbhost_tuh_emit(USBHOST_EV_DETACH, d);
        usbhost_host_detaches++;
    }
    d->in_use = false;
    d->daddr = 0;
    d->desc_len = 0;
}

// -- the host

void usbhost_host_set_class_filter(uint16_t mask) {
    usbhost_tuh_class_filter = mask;
}

// The port is full speed already.
void usbhost_host_set_full_speed(bool full_speed) {
    (void)full_speed;
}

int usbhost_host_start_c(void) {
    if (usbhost_tuh_running) {
        return 0;
    }
    memset(usbhost_tuh_devs, 0, sizeof(usbhost_tuh_devs));
    memset(usbhost_tuh_claims, 0, sizeof(usbhost_tuh_claims));
    for (int i = 0; i < USBHOST_TUH_PIPES; i++) {
        usbhost_tuh_pipe_free(&usbhost_tuh_pipes[i]);
    }
    usbhost_tuh_ctrl.state = USBHOST_TUH_IDLE;
    usbhost_tuh_ctrl.stale = false;
    usbhost_tuh_ctrl.size = 0;
    MP_STATE_VM(usbhost_tuh_bufs)[USBHOST_TUH_CTRL_BUF] = NULL;

    usbhost_tuh_hw_on();
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
    uint32_t st = usbhost_tuh_lock();
    bool ok = tusb_rhport_init(USBHOST_TUH_RHPORT, &init);
    if (ok) {
        usbhost_tuh_running = true;
    }
    usbhost_tuh_unlock(st);
    if (!ok) {
        usbhost_tuh_hw_off();
        return MP_EIO;
    }
    usbhost_tuh_vbus(true);
    soft_timer_static_init(&usbhost_tuh_timer, SOFT_TIMER_MODE_PERIODIC, 1, usbhost_tuh_tick);
    soft_timer_insert(&usbhost_tuh_timer, 1);
    return 0;
}

void usbhost_host_stop_c(void) {
    if (!usbhost_tuh_running) {
        // Power off all the same: the socket is off whenever the host is.
        usbhost_tuh_vbus(false);
        return;
    }
    soft_timer_remove(&usbhost_tuh_timer);
    usbhost_tuh_vbus(false);
    uint32_t st = usbhost_tuh_lock();
    // Unmounts every device, which sends its DETACH and drops its claims and pipes.
    tuh_deinit(USBHOST_TUH_RHPORT);
    usbhost_tuh_running = false;
    for (int i = 0; i < USBHOST_TUH_PIPES; i++) {
        usbhost_tuh_pipe_free(&usbhost_tuh_pipes[i]);
    }
    memset(usbhost_tuh_devs, 0, sizeof(usbhost_tuh_devs));
    memset(usbhost_tuh_claims, 0, sizeof(usbhost_tuh_claims));
    usbhost_tuh_ctrl.gen++;
    usbhost_tuh_ctrl.state = USBHOST_TUH_IDLE;
    usbhost_tuh_ctrl.stale = false;
    usbhost_tuh_ctrl.size = 0;
    MP_STATE_VM(usbhost_tuh_bufs)[USBHOST_TUH_CTRL_BUF] = NULL;
    usbhost_tuh_unlock(st);
    usbhost_tuh_hw_off();
}

// TinyUSB always lets go: nothing to wedge on.
bool usbhost_host_is_wedged(void) {
    return false;
}

bool usbhost_host_is_running(void) {
    return usbhost_tuh_running;
}

int usbhost_host_snapshot(usbhost_event_t *out, int max) {
    int n = 0;
    uint32_t st = usbhost_tuh_lock();
    for (int i = 0; i < USBHOST_TUH_DEVS && n < max; i++) {
        const usbhost_tuh_dev_t *d = &usbhost_tuh_devs[i];
        if (d->in_use) {
            out[n] = (usbhost_event_t) {
                .kind = USBHOST_EV_ATTACH, .speed = d->speed, .vid = d->vid, .pid = d->pid,
                .classes = d->classes, .dev_id = d->daddr,
            };
            n++;
        }
    }
    usbhost_tuh_unlock(st);
    return n;
}

// Devices TinyUSB has configured, hubs included, and one client: usbhost.
int usbhost_host_lib_counts(int *num_devices, int *num_clients) {
    int n = 0;
    uint32_t st = usbhost_tuh_lock();
    for (uint8_t a = 1; usbhost_tuh_running && a <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; a++) {
        n += tuh_mounted(a) ? 1 : 0;
    }
    usbhost_tuh_unlock(st);
    *num_devices = n;
    *num_clients = usbhost_tuh_running ? 1 : 0;
    return 0;
}

// Software replug: the socket's power off for 100 ms.
int usbhost_host_port_cycle(void) {
    if (!usbhost_tuh_running) {
        return -1;
    }
    usbhost_tuh_vbus(false);
    mp_hal_delay_ms(100);
    usbhost_tuh_vbus(true);
    return 0;
}

void usbhost_host_intr_dump(void) {
}

int usbhost_host_desc_get(uint32_t dev_id, const uint8_t **out, uint16_t *len) {
    const usbhost_tuh_dev_t *d = usbhost_tuh_dev_mounted(dev_id);
    if (d == NULL) {
        return -1;
    }
    if (d->desc_len == 0) {
        return -2;
    }
    *out = d->desc;
    *len = d->desc_len;
    return 0;
}

// -- interfaces

static usbhost_tuh_claim_t *usbhost_tuh_find_claim(uint8_t daddr, uint8_t itf) {
    for (int i = 0; i < USBHOST_TUH_CLAIMS; i++) {
        usbhost_tuh_claim_t *c = &usbhost_tuh_claims[i];
        if (c->in_use && c->daddr == daddr && c->itf == itf) {
            return c;
        }
    }
    return NULL;
}

// Open (or close) the bulk and interrupt endpoints of one alternate
// setting. False if the configuration has no such setting, or one won't open.
static bool usbhost_tuh_itf_endpoints(usbhost_tuh_dev_t *d, uint8_t itf, uint8_t alt, bool open) {
    const uint8_t *p = d->desc, *end = d->desc + d->desc_len;
    bool here = false, found = false;
    p += p[0];
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == TUSB_DESC_INTERFACE && p[0] >= 9) {
            here = p[2] == itf && p[3] == alt;
            found |= here;
        } else if (here && p[1] == TUSB_DESC_ENDPOINT && p[0] >= 7 && (p[3] & 0x03) >= TUSB_XFER_BULK) {
            if (open) {
                if (!tuh_edpt_open(d->daddr, (const tusb_desc_endpoint_t *)p)) {
                    return false;
                }
            } else {
                tuh_edpt_close(d->daddr, p[2]);
            }
        }
        p += p[0];
    }
    return found;
}

static void usbhost_tuh_drop_pipes(uint8_t daddr, uint8_t itf) {
    for (int i = 0; i < USBHOST_TUH_PIPES; i++) {
        usbhost_tuh_pipe_t *p = &usbhost_tuh_pipes[i];
        if (p->in_use && p->daddr == daddr && p->itf == itf) {
            usbhost_tuh_pipe_free(p);
        }
    }
}

// 1 for a fresh claim (its endpoints open at DATA0), 0 when the interface was
// already claimed at that alternate, or a negative errno.
int usbhost_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt) {
    uint32_t st = usbhost_tuh_lock();
    int r = 1;
    usbhost_tuh_dev_t *d = usbhost_tuh_dev_mounted(dev_id);
    usbhost_tuh_claim_t *c = d != NULL ? usbhost_tuh_find_claim(d->daddr, itf) : NULL;
    if (d == NULL) {
        r = -MP_ENODEV;
    } else if (c != NULL && c->alt == alt) {
        r = 0;
    } else {
        if (c != NULL) {
            usbhost_tuh_itf_endpoints(d, itf, c->alt, false);
            usbhost_tuh_drop_pipes(d->daddr, itf);
            c->in_use = false;
        } else {
            for (int i = 0; i < USBHOST_TUH_CLAIMS && c == NULL; i++) {
                if (!usbhost_tuh_claims[i].in_use) {
                    c = &usbhost_tuh_claims[i];
                }
            }
        }
        if (c == NULL) {
            r = -MP_ENOMEM;
        } else if (!usbhost_tuh_itf_endpoints(d, itf, alt, true)) {
            usbhost_tuh_itf_endpoints(d, itf, alt, false);
            r = -MP_ENOENT;
        } else {
            *c = (usbhost_tuh_claim_t) { .in_use = true, .daddr = d->daddr, .itf = itf, .alt = alt };
        }
    }
    usbhost_tuh_unlock(st);
    return r;
}

int usbhost_xfer_release(uint32_t dev_id, uint8_t itf) {
    uint32_t st = usbhost_tuh_lock();
    int r = 0;
    usbhost_tuh_dev_t *d = usbhost_tuh_dev_mounted(dev_id);
    if (d == NULL) {
        r = -MP_ENODEV;
    } else {
        usbhost_tuh_claim_t *c = usbhost_tuh_find_claim(d->daddr, itf);
        if (c != NULL) {
            usbhost_tuh_itf_endpoints(d, itf, c->alt, false);
            usbhost_tuh_drop_pipes(d->daddr, itf);
            c->in_use = false;
        }
    }
    usbhost_tuh_unlock(st);
    return r;
}

// -- bulk and interrupt

static int usbhost_tuh_errno(uint8_t result, uint8_t daddr) {
    switch (result) {
        case XFER_RESULT_SUCCESS:
            return 0;
        case XFER_RESULT_STALLED:
            return -MP_EPIPE;
        case XFER_RESULT_TIMEOUT:
        case XFER_RESULT_ABORTED:
            return -MP_ETIMEDOUT;
        default:
            return tuh_mounted(daddr) ? -MP_EIO : -MP_ENODEV;
    }
}

// An abort with the controller's interrupt held off, so the halt it asks
// for can't come in between TinyUSB finding the channel and marking it.
static bool usbhost_tuh_abort(uint8_t daddr, uint8_t ep) {
    hcd_int_disable(USBHOST_TUH_RHPORT);
    bool ok = tuh_edpt_abort_xfer(daddr, ep);
    hcd_int_enable(USBHOST_TUH_RHPORT);
    return ok;
}

static void usbhost_tuh_xfer_cb(tuh_xfer_t *xfer) {
    uint32_t idx = xfer->user_data & 0xFF;
    if (idx >= USBHOST_TUH_PIPES) {
        return;
    }
    usbhost_tuh_pipe_t *p = &usbhost_tuh_pipes[idx];
    if (!p->in_use || p->gen != (uint16_t)(xfer->user_data >> 8) || p->state != USBHOST_TUH_BUSY) {
        return;     // an aborted or freed transfer's
    }
    p->actual = xfer->actual_len;
    p->result = xfer->result;
    __DMB();
    p->state = USBHOST_TUH_DONE;
}

// The endpoint's wMaxPacketSize and interface under the claimed alternate
// setting of its interface: -ENOENT for no such endpoint, -EACCES when its
// interface isn't claimed at a setting that has it.
static int usbhost_tuh_ep_info(const usbhost_tuh_dev_t *d, uint8_t ep, uint16_t *mps, uint8_t *itf_out) {
    const uint8_t *p = d->desc, *end = d->desc + d->desc_len;
    int itf = -1, alt = -1, r = -MP_ENOENT;
    p += p[0];
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == TUSB_DESC_INTERFACE && p[0] >= 9) {
            itf = p[2];
            alt = p[3];
        } else if (p[1] == TUSB_DESC_ENDPOINT && p[0] >= 7 && p[2] == ep && itf >= 0) {
            usbhost_tuh_claim_t *c = usbhost_tuh_find_claim(d->daddr, (uint8_t)itf);
            if (c != NULL && c->alt == alt) {
                *mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
                *itf_out = (uint8_t)itf;
                return 0;
            }
            r = -MP_EACCES;
        }
        p += p[0];
    }
    return r;
}

// With the lock held: the endpoint's pipe, made if it has none.
static usbhost_tuh_pipe_t *usbhost_tuh_pipe(const usbhost_tuh_dev_t *d, uint8_t ep, int *err) {
    usbhost_tuh_pipe_t *free_slot = NULL;
    for (int i = 0; i < USBHOST_TUH_PIPES; i++) {
        usbhost_tuh_pipe_t *p = &usbhost_tuh_pipes[i];
        if (p->in_use && p->daddr == d->daddr && p->ep == ep) {
            return p;
        }
        if (!p->in_use && free_slot == NULL) {
            free_slot = p;
        }
    }
    uint16_t mps;
    uint8_t itf;
    *err = usbhost_tuh_ep_info(d, ep, &mps, &itf);
    if (*err < 0) {
        return NULL;
    }
    if (free_slot == NULL) {
        *err = -MP_ENOMEM;
        return NULL;
    }
    usbhost_tuh_pipe_t *p = free_slot;
    usbhost_tuh_pipe_free(p);
    p->in_use = true;
    p->daddr = d->daddr;
    p->ep = ep;
    p->itf = itf;
    p->mps = mps ? mps : 64;
    return p;
}

// `keep`: an IN transfer that times out stays armed and this returns 0; the
// next call on the endpoint collects it. `now`, with keep: don't wait at all.
static int usbhost_tuh_xfer(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep,
    bool now) {
    const bool in = (ep & 0x80) != 0;
    int r = 0;
    uint32_t st = usbhost_tuh_lock();
    usbhost_tuh_dev_t *d = usbhost_tuh_dev_mounted(dev_id);
    usbhost_tuh_pipe_t *p = d != NULL ? usbhost_tuh_pipe(d, ep, &r) : NULL;
    if (d == NULL) {
        r = -MP_ENODEV;
    }
    if (p == NULL) {
        usbhost_tuh_unlock(st);
        return r;
    }
    const int idx = p - usbhost_tuh_pipes;
    if (p->state == USBHOST_TUH_BUSY && !p->kept) {
        usbhost_tuh_unlock(st);
        return -MP_EBUSY;
    }
    if (!p->kept) {
        // IN transfers are whole packets.
        uint32_t size = in ? (len ? (len + p->mps - 1) / p->mps * p->mps : p->mps) : len;
        if (size > p->size) {
            // Allocate with the lock let go: a collection can take a while,
            // and a MemoryError mustn't leave PendSV held off.
            const uint16_t gen = p->gen;
            usbhost_tuh_unlock(st);
            void *buf = m_malloc_maybe(size);
            if (buf == NULL) {
                return -MP_ENOMEM;
            }
            st = usbhost_tuh_lock();
            if (!p->in_use || p->gen != gen) {
                usbhost_tuh_unlock(st);
                return -MP_ENODEV;      // gone meanwhile
            }
            MP_STATE_VM(usbhost_tuh_bufs)[idx] = buf;
            p->size = size;
        }
        uint8_t *buf = MP_STATE_VM(usbhost_tuh_bufs)[idx];
        if (!in && len) {
            memcpy(buf, data, len);
        }
        p->gen++;
        p->actual = 0;
        p->result = XFER_RESULT_INVALID;
        p->state = USBHOST_TUH_BUSY;
        tuh_xfer_t x = {
            .daddr = (uint8_t)dev_id,
            .ep_addr = ep,
            .buflen = in ? size : len,
            .buffer = len || in ? buf : NULL,
            .complete_cb = usbhost_tuh_xfer_cb,
            .user_data = (uintptr_t)idx | ((uintptr_t)p->gen << 8),
        };
        if (!tuh_edpt_xfer(&x)) {
            p->state = USBHOST_TUH_IDLE;
            usbhost_tuh_unlock(st);
            return tuh_mounted((uint8_t)dev_id) ? -MP_EIO : -MP_ENODEV;
        }
    }
    p->kept = false;
    p->waiter = true;
    usbhost_tuh_unlock(st);

    bool done = now ? p->state == USBHOST_TUH_DONE : usbhost_tuh_wait(&p->state, timeout_ms);

    st = usbhost_tuh_lock();
    p->waiter = false;
    if (p->dead) {
        usbhost_tuh_pipe_free(p);
        usbhost_tuh_unlock(st);
        return -MP_ENODEV;
    }
    if (!done) {
        // A completion the interrupt has queued and the task hasn't run yet:
        // run the task here (PendSV is held off, so this is the only one),
        // so a note that lands as a read gives up isn't dropped.
        tuh_task_ext(0, false);
        done = p->state == USBHOST_TUH_DONE;
    }
    if (!done) {
        if (keep && in) {
            p->kept = true;
            usbhost_tuh_unlock(st);
            return 0;
        }
        usbhost_tuh_abort((uint8_t)dev_id, ep);
        p->gen++;
        p->state = USBHOST_TUH_IDLE;
        usbhost_tuh_unlock(st);
        return -MP_ETIMEDOUT;
    }
    p->state = USBHOST_TUH_IDLE;
    r = usbhost_tuh_errno(p->result, (uint8_t)dev_id);
    if (r == 0) {
        r = (int)p->actual;
        if (in) {
            if ((size_t)r > len) {
                r = (int)len;   // the rest is dropped, as libusb reports overflow
            }
            memcpy(data, MP_STATE_VM(usbhost_tuh_bufs)[idx], (size_t)r);
        }
    } else if (r == -MP_EPIPE) {
        hcd_edpt_clear_stall(USBHOST_TUH_RHPORT, (uint8_t)dev_id, ep);
    }
    usbhost_tuh_unlock(st);
    return r;
}

int usbhost_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep) {
    return usbhost_tuh_xfer(dev_id, ep, data, len, timeout_ms, keep, false);
}

int usbhost_xfer_poll(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len) {
    if (!(ep & 0x80)) {
        return -MP_EINVAL;
    }
    return usbhost_tuh_xfer(dev_id, ep, data, len, 0, true, true);
}

// -- control

static void usbhost_tuh_ctrl_cb(tuh_xfer_t *xfer) {
    if ((uint16_t)xfer->user_data != usbhost_tuh_ctrl.gen) {
        usbhost_tuh_ctrl.stale = false;     // an abandoned one, done at last
        return;
    }
    usbhost_tuh_ctrl.actual = xfer->actual_len;
    usbhost_tuh_ctrl.result = xfer->result;
    __DMB();
    usbhost_tuh_ctrl.state = USBHOST_TUH_DONE;
}

int usbhost_xfer_ctrl(uint32_t dev_id, const uint8_t setup[8], uint8_t *data, uint16_t len, uint32_t timeout_ms) {
    if (usbhost_tuh_ctrl.state == USBHOST_TUH_BUSY || usbhost_tuh_ctrl.stale) {
        return -MP_EBUSY;
    }
    if (len > usbhost_tuh_ctrl.size) {
        void *buf = m_malloc_maybe(len < 64 ? 64 : len);
        if (buf == NULL) {
            return -MP_ENOMEM;
        }
        MP_STATE_VM(usbhost_tuh_bufs)[USBHOST_TUH_CTRL_BUF] = buf;
        usbhost_tuh_ctrl.size = len < 64 ? 64 : len;
    }
    uint8_t *buf = MP_STATE_VM(usbhost_tuh_bufs)[USBHOST_TUH_CTRL_BUF];
    const bool in = (setup[0] & 0x80) != 0;
    uint32_t st = usbhost_tuh_lock();
    if (usbhost_tuh_dev_mounted(dev_id) == NULL) {
        usbhost_tuh_unlock(st);
        return -MP_ENODEV;
    }
    memcpy(&usbhost_tuh_ctrl.request, setup, 8);
    if (!in && len) {
        memcpy(buf, data, len);
    }
    usbhost_tuh_ctrl.gen++;
    usbhost_tuh_ctrl.daddr = (uint8_t)dev_id;
    usbhost_tuh_ctrl.actual = 0;
    usbhost_tuh_ctrl.result = XFER_RESULT_INVALID;
    usbhost_tuh_ctrl.state = USBHOST_TUH_BUSY;
    tuh_xfer_t x = {
        .daddr = (uint8_t)dev_id,
        .ep_addr = 0,
        .setup = &usbhost_tuh_ctrl.request,
        .buffer = len ? buf : NULL,
        .complete_cb = usbhost_tuh_ctrl_cb,
        .user_data = usbhost_tuh_ctrl.gen,
    };
    if (!tuh_control_xfer(&x)) {
        usbhost_tuh_ctrl.state = USBHOST_TUH_IDLE;
        usbhost_tuh_unlock(st);
        return tuh_mounted((uint8_t)dev_id) ? -MP_EIO : -MP_ENODEV;
    }
    usbhost_tuh_unlock(st);

    bool done = usbhost_tuh_wait(&usbhost_tuh_ctrl.state, timeout_ms);

    st = usbhost_tuh_lock();
    if (!done) {
        tuh_task_ext(0, false);
        done = usbhost_tuh_ctrl.state == USBHOST_TUH_DONE;
    }
    if (!done) {
        // Abandon it: a newer generation drops its completion. If TinyUSB
        // can't abort it (queued behind another device's), it keeps the
        // buffer until it completes, and the next control transfer waits.
        usbhost_tuh_ctrl.gen++;
        usbhost_tuh_ctrl.stale = true;
        if (usbhost_tuh_abort((uint8_t)dev_id, 0)) {
            usbhost_tuh_ctrl.stale = false;
        }
        usbhost_tuh_ctrl.state = USBHOST_TUH_IDLE;
        usbhost_tuh_unlock(st);
        return -MP_ETIMEDOUT;
    }
    usbhost_tuh_ctrl.state = USBHOST_TUH_IDLE;
    int r = usbhost_tuh_errno(usbhost_tuh_ctrl.result, (uint8_t)dev_id);
    if (r == 0) {
        uint32_t got = usbhost_tuh_ctrl.actual < len ? usbhost_tuh_ctrl.actual : len;
        if (in) {
            memcpy(data, buf, got);
        }
        r = in ? (int)got : len;
    }
    usbhost_tuh_unlock(st);
    return r;
}

#endif // USBHOST_ENGINE_TUH
