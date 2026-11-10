// SPDX-License-Identifier: MIT
//
// Host-side tests for TinyUSB's enumeration on a root port, as the Arduino
// GIGA R1's USB-A port runs it: TinyUSB's own usbh.c and hub.c, with
// patches/lib-tinyusb/ applied, over a fake controller and a fake device.
//
// A USB flash drive plugged straight into the port doesn't answer its first
// requests: it is still starting up when the host resets it and asks for its
// device descriptor. TinyUSB gave up after that one attempt and never tried
// again, so the drive stayed connected and was never listed. With
// patches/lib-tinyusb/0002 the root port is reset and enumerated again, up to
// three more times.
//
// Built against TinyUSB without that patch, this test has to fail (CI checks
// that it does).
#include <stdio.h>
#include <string.h>

#include "tusb.h"
#include "host/hcd.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

// -- the clock

static uint32_t now_ms;

uint32_t tusb_time_millis_api(void) {
    return now_ms;
}

// -- the fake device

static const uint8_t desc_device[18] = {
    18, TUSB_DESC_DEVICE, 0x00, 0x02, 0, 0, 0, 64,
    0x81, 0x07, 0x81, 0x55,     // 0781:5581, a SanDisk drive
    0x00, 0x01, 0, 0, 0, 1,
};

static const uint8_t desc_config[32] = {
    9, TUSB_DESC_CONFIGURATION, 32, 0, 1, 1, 0, 0x80, 50,
    9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_MSC, 6, 0x50, 0,
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, 0x02, TUSB_XFER_BULK, 64, 0, 0,
};

static const uint8_t desc_langid[4] = { 4, TUSB_DESC_STRING, 0x09, 0x04 };

static struct {
    bool plugged;
    uint32_t powered_at;        // when it was plugged in
    uint32_t ready_after_ms;    // how long it ignores requests after that; 0xFFFFFFFF: always
    uint8_t addr;
    const uint8_t *reply;       // the data stage's
    uint16_t reply_len;
} dev;

// What the host did.
static uint32_t resets, setups, setups_ignored, dev0_opens, dev0_closes;
static uint8_t mounted;

static void plug(uint32_t ready_after_ms) {
    memset(&dev, 0, sizeof(dev));
    dev.plugged = true;
    dev.powered_at = now_ms;
    dev.ready_after_ms = ready_after_ms;
    hcd_event_device_attach(0, false);
}

static void unplug(void) {
    dev.plugged = false;
    hcd_event_device_remove(0, false);
}

static bool dev_ready(void) {
    return dev.ready_after_ms != 0xFFFFFFFF && now_ms - dev.powered_at >= dev.ready_after_ms;
}

// -- the fake controller: every transfer completes on the next task run

bool hcd_init(uint8_t rhport, const tusb_rhport_init_t *rh_init) {
    (void)rhport;
    (void)rh_init;
    return true;
}

void hcd_int_handler(uint8_t rhport, bool in_isr) {
    (void)rhport;
    (void)in_isr;
}

void hcd_int_enable(uint8_t rhport) {
    (void)rhport;
}

void hcd_int_disable(uint8_t rhport) {
    (void)rhport;
}

uint32_t hcd_frame_number(uint8_t rhport) {
    (void)rhport;
    return now_ms;
}

bool hcd_port_connect_status(uint8_t rhport) {
    (void)rhport;
    return dev.plugged;
}

void hcd_port_reset(uint8_t rhport) {
    (void)rhport;
    resets++;
    dev.addr = 0;
}

void hcd_port_reset_end(uint8_t rhport) {
    (void)rhport;
}

tusb_speed_t hcd_port_speed_get(uint8_t rhport) {
    (void)rhport;
    return TUSB_SPEED_FULL;
}

void hcd_device_close(uint8_t rhport, uint8_t dev_addr) {
    (void)rhport;
    if (dev_addr == 0) {
        dev0_closes++;
    }
}

bool hcd_edpt_open(uint8_t rhport, uint8_t daddr, tusb_desc_endpoint_t const *ep_desc) {
    (void)rhport;
    if (daddr == 0 && ep_desc->bEndpointAddress == 0) {
        dev0_opens++;
    }
    return true;
}

bool hcd_edpt_close(uint8_t rhport, uint8_t daddr, uint8_t ep_addr) {
    (void)rhport;
    (void)daddr;
    (void)ep_addr;
    return true;
}

bool hcd_setup_send(uint8_t rhport, uint8_t daddr, uint8_t const setup_packet[8]) {
    (void)rhport;
    setups++;
    if (!dev.plugged || !dev_ready() || daddr != dev.addr) {
        // No handshake: the controller gives up after its own retries.
        setups_ignored++;
        hcd_event_xfer_complete(daddr, 0x00, 0, XFER_RESULT_FAILED, false);
        return true;
    }
    const tusb_control_request_t *req = (const tusb_control_request_t *)setup_packet;
    xfer_result_t result = XFER_RESULT_SUCCESS;
    dev.reply = NULL;
    dev.reply_len = 0;
    if (req->bRequest == TUSB_REQ_GET_DESCRIPTOR) {
        switch (req->wValue >> 8) {
            case TUSB_DESC_DEVICE:
                dev.reply = desc_device;
                dev.reply_len = sizeof(desc_device);
                break;
            case TUSB_DESC_CONFIGURATION:
                dev.reply = desc_config;
                dev.reply_len = sizeof(desc_config);
                break;
            case TUSB_DESC_STRING:
                dev.reply = desc_langid;
                dev.reply_len = sizeof(desc_langid);
                break;
            default:
                result = XFER_RESULT_STALLED;
                break;
        }
        if (dev.reply_len > req->wLength) {
            dev.reply_len = req->wLength;
        }
    } else if (req->bRequest == TUSB_REQ_SET_ADDRESS) {
        dev.addr = (uint8_t)req->wValue;
    } else if (req->bRequest != TUSB_REQ_SET_CONFIGURATION) {
        result = XFER_RESULT_STALLED;
    }
    hcd_event_xfer_complete(daddr, 0x00, 8, result, false);
    return true;
}

bool hcd_edpt_xfer(uint8_t rhport, uint8_t daddr, uint8_t ep_addr, uint8_t *buffer, uint16_t buflen) {
    (void)rhport;
    uint16_t n = 0;
    if (buffer != NULL && buflen > 0 && (ep_addr & TUSB_DIR_IN_MASK) && dev.reply != NULL) {
        n = dev.reply_len < buflen ? dev.reply_len : buflen;
        memcpy(buffer, dev.reply, n);
    }
    hcd_event_xfer_complete(daddr, ep_addr, n, dev.plugged ? XFER_RESULT_SUCCESS : XFER_RESULT_FAILED, false);
    return true;
}

bool hcd_edpt_abort_xfer(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
    (void)rhport;
    (void)dev_addr;
    (void)ep_addr;
    return true;
}

bool hcd_edpt_clear_stall(uint8_t rhport, uint8_t dev_addr, uint8_t ep_addr) {
    (void)rhport;
    (void)dev_addr;
    (void)ep_addr;
    return true;
}

// -- the application's callbacks

void tuh_mount_cb(uint8_t daddr) {
    mounted = daddr;
}

void tuh_umount_cb(uint8_t daddr) {
    if (mounted == daddr) {
        mounted = 0;
    }
}

// -- the tests

static void run_for(uint32_t ms) {
    for (uint32_t end = now_ms + ms; now_ms < end; now_ms++) {
        for (int i = 0; i < 4; i++) {
            tuh_task_ext(0, false);
        }
    }
}

static void reset_counts(void) {
    resets = setups = setups_ignored = dev0_opens = dev0_closes = 0;
}

// A drive that ignores the host for its first 300 ms, as the GIGA's flash
// drive does: the first reset comes 150 ms after the plug, so the first
// attempt fails, and a retry has to find it ready.
static void test_slow_drive_mounts(void) {
    reset_counts();
    plug(300);
    run_for(3000);
    printf("slow drive: %u resets, %u setups (%u ignored), mounted at address %u\n",
        (unsigned)resets, (unsigned)setups, (unsigned)setups_ignored, mounted);
    CHECK(setups_ignored > 0);
    CHECK(mounted != 0);
    CHECK(resets >= 2);
    CHECK(dev0_opens == dev0_closes);
    unplug();
    run_for(50);
    CHECK(mounted == 0);
}

// A device that never answers: the host stops after its retries, rather than
// resetting the port for good, and leaves no endpoint open.
static void test_dead_device_stops(void) {
    reset_counts();
    plug(0xFFFFFFFF);
    run_for(5000);
    uint32_t resets_then = resets;
    run_for(5000);
    printf("dead device: %u resets, %u setups\n", (unsigned)resets, (unsigned)setups);
    CHECK(mounted == 0);
    CHECK(resets == resets_then);
    CHECK(resets <= 4);
    CHECK(dev0_opens == dev0_closes);
    unplug();
    run_for(50);
}

// A device that answers at once mounts on the first attempt, after a dead one
// used up its retries.
static void test_ready_device_after_dead_one(void) {
    reset_counts();
    plug(0);
    run_for(1000);
    printf("ready device: %u resets, mounted at address %u\n", (unsigned)resets, mounted);
    CHECK(mounted != 0);
    CHECK(resets == 1);
    unplug();
    run_for(50);
}

int main(void) {
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL };
    if (!tusb_rhport_init(0, &init)) {
        printf("FAIL: tusb_rhport_init\n");
        return 1;
    }
    test_slow_drive_mounts();
    test_dead_device_stops();
    test_ready_device_after_dead_one();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
