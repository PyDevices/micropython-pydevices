// SPDX-License-Identifier: MIT
//
// The `_usbif` native module: the MicroPython-facing surface of usbif.
//
// The host port (ESP-IDF's USB host library, usbif_host.c) and what usb.core
// and the usb.host.* packages build on: the device list and descriptors,
// generic control, bulk and interrupt transfers (usbif_host_xfer.c),
// isochronous streams (usbif_host_stream.c) and the tethering adapter
// (usbif_host_net.c). The class drivers are those Python packages; this module
// is their only caller.

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"
#include <stdio.h>
#include <string.h>

#include "shared/usbif_ringbuf.h"

#if MICROPY_HW_ENABLE_USBDEV
#include "tusb.h"
#include "mp_usbd.h"
#endif

#include "usbif_classes.h"
#include "pcm_c_sink.h"

// Host engine availability: the IDF USB Host Library, on chips with an OTG
// controller. The same gate guards usbif_host.c; elsewhere the host_* calls
// keep their Phase-1 stub behaviour (an honest empty capability set).
// Usermod sources build with MicroPython's define set, not the IDF's, so
// ESP_PLATFORM is NOT defined here -- learned the hard way when this gate
// silently compiled the engine out and the Phase 1 stubs answered in its
// place. sdkconfig.h existing (and naming an OTG controller) is the real
// signal that the IDF host library is available.
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif
#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED
#define USBIF_HAVE_HOST (1)
extern int usbif_host_start_c(void);
extern void usbif_host_stop_c(void);
extern bool usbif_host_is_wedged(void);
extern bool usbif_host_is_running(void);
extern int usbif_host_snapshot(usbif_event_t *out, int max);
extern uint32_t usbif_host_attaches, usbif_host_detaches, usbif_host_errors;
extern int usbif_host_lib_counts(int *num_devices, int *num_clients);
extern int usbif_host_port_cycle(void);
extern void usbif_host_set_class_filter(uint16_t mask);
extern void usbif_host_set_full_speed(bool full_speed);
extern void usbif_host_intr_dump(void);
extern int usbif_host_desc_get(uint32_t dev_id, const uint8_t **out, uint16_t *len);
#else
#define USBIF_HAVE_HOST (0)
#endif

// Event capacity. Sized from measurement rather than a round number: the worst
// VM stall observed on an ESP32-S3 was 1537 ms during flash writes, and a
// 1 kHz event stream over that window is ~1500 events. Full HID/MIDI traffic
// at that rate through a stall this long is a pathological case rather than a
// working one, so the ring holds a generous fraction of it and reports
// overflow instead of pretending. 256 records is 2 KB at 8 bytes each.
#define USBIF_EVENT_CAPACITY (256)

static usbif_event_t usbif_event_slots[USBIF_EVENT_CAPACITY];
static usbif_ringbuf_t usbif_events;
static bool usbif_host_running = false;

// Called by anything that starts hardware a soft reset must stop; see
// "Soft reset" at the end of this file.
static void usbif_arm_soft_reset(void);

// Producer hook for the host engine (usbif_host.c). Called only from the
// host task, preserving the ring's single-producer contract.
void usbif_host_emit(const usbif_event_t *event) {
    usbif_rb_push(&usbif_events, event);
}

static const struct {
    uint16_t bit;
    qstr name;
} usbif_class_names[] = {
    { USBIF_CLASS_HID, MP_QSTR_hid },
    { USBIF_CLASS_MSC, MP_QSTR_msc },
    { USBIF_CLASS_CDC, MP_QSTR_cdc },
    { USBIF_CLASS_MIDI, MP_QSTR_midi },
    { USBIF_CLASS_UAC, MP_QSTR_uac },
    { USBIF_CLASS_UVC, MP_QSTR_uvc },
    { USBIF_CLASS_OTHER, MP_QSTR_other },
};

static const qstr usbif_speed_names[] = {
    MP_QSTRnull, MP_QSTR_low, MP_QSTR_full, MP_QSTR_high,
};

// Classes a host_start() may ask for: every class is driven from Python,
// over usb.core's transfers and the isochronous streams, so all of them
// wherever there is a host, and none where there isn't.
static uint16_t usbif_supported_classes(void) {
    #if USBIF_HAVE_HOST
    return USBIF_CLASS_CDC | USBIF_CLASS_HID | USBIF_CLASS_MSC | USBIF_CLASS_MIDI
        | USBIF_CLASS_UAC | USBIF_CLASS_UVC | USBIF_CLASS_OTHER;
    #else
    return 0;
    #endif
}

static mp_obj_t usbif_classes_to_set(uint16_t mask) {
    mp_obj_t set = mp_obj_new_set(0, NULL);
    for (size_t i = 0; i < MP_ARRAY_SIZE(usbif_class_names); i++) {
        if (mask & usbif_class_names[i].bit) {
            mp_obj_set_store(set, MP_OBJ_NEW_QSTR(usbif_class_names[i].name));
        }
    }
    return set;
}

// Reverse of usbif_classes_to_set: an iterable of class-name strings (a tuple,
// as usb.core.start() passes it) to a bitmask. Unknown names are ignored
// rather than raising -- host_start() intersects the result against
// usbif_supported_classes() anyway, so an unrecognised or unsupported name
// simply doesn't start, which is what "not built into this firmware" should
// look like on the host side too.
static uint16_t usbif_classes_from_iterable(mp_obj_t classes_in) {
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(classes_in, &len, &items);
    uint16_t mask = 0;
    for (size_t j = 0; j < len; j++) {
        qstr name = mp_obj_str_get_qstr(items[j]);
        for (size_t i = 0; i < MP_ARRAY_SIZE(usbif_class_names); i++) {
            if (usbif_class_names[i].name == name) {
                mask |= usbif_class_names[i].bit;
                break;
            }
        }
    }
    return mask;
}

// One row of host_devices(): (dev_id, vid, pid, product, serial, classes,
// speed). usb.core's Device reads it by position, so the order is its
// contract.
static mp_obj_t usbif_device_row(const usbif_event_t *event) {
    mp_obj_t items[7];
    items[0] = mp_obj_new_int_from_uint(event->dev_id);
    items[1] = mp_obj_new_int_from_uint(event->vid);
    items[2] = mp_obj_new_int_from_uint(event->pid);
    items[3] = mp_const_none;   // product: filled by the class drivers
    items[4] = mp_const_none;   // serial
    items[5] = usbif_classes_to_set(event->classes);
    items[6] = event->speed < MP_ARRAY_SIZE(usbif_speed_names) && event->speed
        ? MP_OBJ_NEW_QSTR(usbif_speed_names[event->speed])
        : mp_const_none;
    return mp_obj_new_tuple(7, items);
}

// host_start(classes, *, full_speed=False). full_speed holds the root port
// to full speed, so full- and low-speed devices work behind a hub on the
// P4's high-speed host (usbif#15); see usbif_host_set_full_speed(). Like the
// class filter, it takes effect when the host starts, not on a running host.
static mp_obj_t usbif_host_start(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_classes, ARG_full_speed };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_classes, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = mp_const_none } },
        { MP_QSTR_full_speed, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    mp_obj_t classes_in = args[ARG_classes].u_obj;
    // Intersected against what this firmware can actually drive: a caller
    // naming an unbuilt or unknown class doesn't start it, matching the
    // return value ("the set actually started") that usb.core relies on.
    uint16_t wanted = usbif_classes_from_iterable(classes_in) & usbif_supported_classes();
    // Only a genuine start initialises the ring: a second host_start() on a
    // running host must be a no-op, not a wipe of queued events -- learned
    // when the NUCLEO's attach events vanished under an idempotent restart.
    if (!usbif_host_running) {
        usbif_rb_init(&usbif_events, usbif_event_slots, USBIF_EVENT_CAPACITY);
    }
    #if USBIF_HAVE_HOST
    if (!usbif_host_running) {
        usbif_host_set_full_speed(args[ARG_full_speed].u_bool);
    }
    usbif_host_set_class_filter(wanted);
    int err = usbif_host_start_c();
    if (err != 0) {
        if (usbif_host_is_wedged()) {
            // A plain errno here read as 0x103 or EIO, which says nothing
            // about the one thing that clears it.
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("USB host wedged: a device could not be released; reset the board"));
        }
        mp_raise_OSError(err > 0 ? err : MP_EIO);
    }
    #endif
    usbif_host_running = true;
    usbif_arm_soft_reset();
    return usbif_classes_to_set(wanted);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbif_host_start_obj, 1, usbif_host_start);

// A running host stream's ring, kept reachable here so a collection can't
// free it under the transfers. Index = the stream's slot (USBIF_HOST_STREAMS).
MP_REGISTER_ROOT_POINTER(mp_obj_t usbif_stream_owner[4]);
#if USBIF_HAVE_HOST
static void usbif_stream_release_owners(void);
#endif

static mp_obj_t usbif_host_stop(void) {
    #if USBIF_HAVE_HOST
    usbif_host_stop_c();
    usbif_stream_release_owners();
    #endif
    usbif_host_running = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_stop_obj, usbif_host_stop);

static mp_obj_t usbif_host_devices(void) {
    #if USBIF_HAVE_HOST
    usbif_event_t rows[8];
    int n = usbif_host_snapshot(rows, 8);
    mp_obj_t items[8];
    for (int i = 0; i < n; i++) {
        items[i] = usbif_device_row(&rows[i]);
    }
    return mp_obj_new_tuple(n, items);
    #else
    return mp_obj_new_tuple(0, NULL);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_devices_obj, usbif_host_devices);

// Pop up to `limit` records and report whether anything was lost since the
// last call. Bounded so one poll cannot turn a burst into an unbounded
// allocation on a device with a small heap; whatever is left stays queued.
static mp_obj_t usbif_host_drain(mp_obj_t limit_in) {
    mp_int_t limit = mp_obj_get_int(limit_in);
    if (limit < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("limit must not be negative"));
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    usbif_event_t event;
    while (limit-- > 0 && usbif_rb_pop(&usbif_events, &event)) {
        mp_obj_t pair[2] = {
            MP_OBJ_NEW_SMALL_INT(event.kind),
            usbif_device_row(&event),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(2, pair));
    }

    mp_obj_t result[2] = {
        list,
        mp_obj_new_bool(usbif_rb_take_dropped(&usbif_events) != 0),
    };
    return mp_obj_new_tuple(2, result);
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_drain_obj, usbif_host_drain);

// Diagnostic: (running, attaches, detaches, errors, lib_devices, lib_clients).
static mp_obj_t usbif_host_stats(void) {
    #if USBIF_HAVE_HOST
    int devs = -1, clients = -1;
    usbif_host_lib_counts(&devs, &clients);
    mp_obj_t items[6] = {
        mp_obj_new_bool(usbif_host_is_running()),
        mp_obj_new_int_from_uint(usbif_host_attaches),
        mp_obj_new_int_from_uint(usbif_host_detaches),
        mp_obj_new_int_from_uint(usbif_host_errors),
        MP_OBJ_NEW_SMALL_INT(devs),
        MP_OBJ_NEW_SMALL_INT(clients),
    };
    return mp_obj_new_tuple(6, items);
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_stats_obj, usbif_host_stats);

// Software replug: power-cycle the root port to force fresh connect detection.
static mp_obj_t usbif_host_port_cycle_py(void) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_host_port_cycle());
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_port_cycle_obj, usbif_host_port_cycle_py);

static mp_obj_t usbif_host_intr_dump_py(void) {
    #if USBIF_HAVE_HOST
    usbif_host_intr_dump();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_intr_dump_obj, usbif_host_intr_dump_py);

// The raw active configuration descriptor of a hosted device.
//
// Everything a class needs to describe itself is in here -- interfaces, alt
// settings, endpoints, and the class-specific descriptors that carry audio
// formats and video frame tables. Returned whole so the parsing happens in
// Python, where a wrong guess costs a re-run rather than a reflash.
static mp_obj_t usbif_host_desc_py(mp_obj_t dev_id_in) {
    #if USBIF_HAVE_HOST
    const uint8_t *desc = NULL;
    uint16_t len = 0;
    int rc = usbif_host_desc_get((uint32_t)mp_obj_get_int(dev_id_in), &desc, &len);
    if (rc != 0) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_desc failed (%d): -1 no such device, -2 no active config"), rc);
    }
    return mp_obj_new_bytes(desc, len);
    #else
    (void)dev_id_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_desc_obj, usbif_host_desc_py);

// Generic transfers, the C half of usb.core (usbif_host_xfer.c). Each blocks
// until its transfer completes or times out, and raises OSError with the
// errno: ETIMEDOUT, EPIPE for a stall, ENODEV once the device is gone,
// EACCES for an endpoint whose interface isn't claimed.
#if USBIF_HAVE_HOST
extern int usbif_xfer_claim(uint32_t dev_id, uint8_t itf, uint8_t alt);
extern int usbif_xfer_release(uint32_t dev_id, uint8_t itf);
extern int usbif_xfer_ctrl(uint32_t dev_id, const uint8_t setup[8], uint8_t *data, uint16_t len, uint32_t timeout_ms);
extern int usbif_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms, bool keep);

static mp_obj_t usbif_xfer_check(int r) {
    if (r < 0) {
        mp_raise_OSError(-r);
    }
    return MP_OBJ_NEW_SMALL_INT(r);
}
#endif

// host_claim(dev_id, interface, alternate=0): True for a fresh claim, whose
// pipes start at DATA0; False when it was already claimed.
static mp_obj_t usbif_host_claim_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    uint8_t alt = n_args > 2 ? (uint8_t)mp_obj_get_int(args[2]) : 0;
    int r = usbif_xfer_claim((uint32_t)mp_obj_get_int(args[0]), (uint8_t)mp_obj_get_int(args[1]), alt);
    usbif_xfer_check(r);
    return mp_obj_new_bool(r == 1);
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_claim_obj, 2, 3, usbif_host_claim_py);

// host_release(dev_id, interface)
static mp_obj_t usbif_host_release_py(mp_obj_t dev_id_in, mp_obj_t itf_in) {
    #if USBIF_HAVE_HOST
    usbif_xfer_check(usbif_xfer_release((uint32_t)mp_obj_get_int(dev_id_in), (uint8_t)mp_obj_get_int(itf_in)));
    return mp_const_none;
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_release_obj, usbif_host_release_py);

// host_ctrl(dev_id, setup, buf, timeout_ms): setup is the 8-byte SETUP
// packet; buf receives an IN request's data (wLength bytes) or holds an OUT
// request's. Returns the bytes moved.
static mp_obj_t usbif_host_ctrl_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t setup, buf = { .buf = NULL, .len = 0 };
    mp_get_buffer_raise(args[1], &setup, MP_BUFFER_READ);
    if (setup.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("a SETUP packet is 8 bytes"));
    }
    const uint8_t *sp = setup.buf;
    uint16_t wlength = (uint16_t)(sp[6] | (sp[7] << 8));
    if (args[2] != mp_const_none) {
        mp_get_buffer_raise(args[2], &buf, (sp[0] & 0x80) ? MP_BUFFER_WRITE : MP_BUFFER_READ);
    }
    if (buf.len < wlength) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer shorter than wLength"));
    }
    return usbif_xfer_check(usbif_xfer_ctrl((uint32_t)mp_obj_get_int(args[0]), sp, buf.buf, wlength,
        (uint32_t)mp_obj_get_int(args[3])));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_ctrl_obj, 4, 4, usbif_host_ctrl_py);

// host_xfer(dev_id, endpoint, buf, timeout_ms, raise_on_timeout=True): a
// bulk or interrupt transfer; an IN endpoint reads into buf, an OUT one sends
// it. Returns the bytes moved. timeout_ms 0 waits for good. With
// raise_on_timeout False, an IN read that times out returns 0, allocating
// nothing, and leaves its transfer armed for the next read to collect.
static mp_obj_t usbif_host_xfer_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    uint8_t ep = (uint8_t)mp_obj_get_int(args[1]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[2], &buf, (ep & 0x80) ? MP_BUFFER_WRITE : MP_BUFFER_READ);
    bool keep = n_args > 4 && !mp_obj_is_true(args[4]);
    return usbif_xfer_check(usbif_xfer_ep((uint32_t)mp_obj_get_int(args[0]), ep, buf.buf, buf.len,
        (uint32_t)mp_obj_get_int(args[3]), keep));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_xfer_obj, 4, 5, usbif_host_xfer_py);

// Isochronous host streams (usbif_host_stream.c): an endpoint's packets moved
// to or from a ring in C, for usb.host.audio and usb.host.video. The ring is
// a machine.USBDevice.Ring, the same one a device-side stream uses, or, for
// a capture stream, an audiodsp audiopump.Ring the pump plays from.
#if USBIF_HAVE_HOST
#include "usbif_host_stream.h"

#if defined(__has_include)
#if __has_include("audiopump/audiopump_ring.h")
#include "audiopump/audiopump_ring.h"
#endif
#endif

static void usbif_stream_release_owners(void) {
    uint32_t free_slots = usbif_stream_free_slots();
    for (int i = 0; i < USBIF_HOST_STREAMS; i++) {
        if (free_slots & (1u << i)) {
            MP_STATE_VM(usbif_stream_owner)[i] = MP_OBJ_NULL;
        }
    }
}

#if defined(MP_USBD_HAS_RING_API)
static uint32_t usbif_ring_put(void *r, const uint8_t *src, uint32_t n) {
    return mp_usbd_ring_put(r, src, n) ? n : 0;
}
#endif
#endif

// host_stream(dev_id, ep, ring, packet=0, *, rate=0, frame=0, video=False,
// max_frame=0): start moving ep's packets to (IN) or from (OUT) ring. The
// endpoint's interface must be claimed at its streaming alternate setting.
// rate and frame pace a PCM OUT stream; video assembles UVC frames, each led
// in the ring by its 4-byte length. host_stream(dev_id, ep, None) stops it.
static mp_obj_t usbif_host_stream_py(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_dev, ARG_ep, ARG_ring, ARG_packet, ARG_rate, ARG_frame, ARG_video, ARG_max_frame };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_dev, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_ep, MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_ring, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_packet, MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_rate, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_frame, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_video, MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
        { MP_QSTR_max_frame, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    #if USBIF_HAVE_HOST
    uint32_t dev_id = (uint32_t)args[ARG_dev].u_int;
    uint8_t ep = (uint8_t)args[ARG_ep].u_int;
    mp_obj_t ring = args[ARG_ring].u_obj;
    if (ring == mp_const_none) {
        int r = usbif_stream_close(dev_id, ep);
        usbif_stream_release_owners();
        if (r < 0 && r != -MP_ENOENT) {
            mp_raise_OSError(-r);
        }
        return mp_const_none;
    }
    usbif_stream_cfg_t cfg = {
        .packet = (uint16_t)args[ARG_packet].u_int,
        .rate = (uint32_t)args[ARG_rate].u_int,
        .frame = (uint16_t)args[ARG_frame].u_int,
        .video = args[ARG_video].u_bool,
        .max_frame = (uint32_t)args[ARG_max_frame].u_int,
    };
    void *ptr = MP_OBJ_TO_PTR(ring);
    bool known = false;
    #if defined(MP_USBD_HAS_RING_API)
    if (mp_obj_is_type(ring, &mp_usbd_ring_type)) {
        cfg.ring = (usbif_stream_ring_t) {
            .ctx = ptr, .put = usbif_ring_put, .get = mp_usbd_ring_get,
            .used = mp_usbd_ring_used, .size = mp_usbd_ring_size,
            .stage = mp_usbd_ring_stage, .commit = mp_usbd_ring_commit,
            .overflow = mp_usbd_ring_overflow,
        };
        known = true;
    }
    #endif
    #if defined(AUDIOPUMP_RING_HAS_SINK_WRITE)
    if (mp_obj_is_type(ring, &audiopump_ring_type)) {
        if (!(ep & 0x80) || cfg.video) {
            mp_raise_ValueError(MP_ERROR_TEXT("an audiopump.Ring takes a capture (IN) stream"));
        }
        cfg.ring = (usbif_stream_ring_t) { .ctx = ptr, .put = audiopump_ring_sink_write };
        known = true;
    }
    #endif
    if (!known) {
        mp_raise_TypeError(MP_ERROR_TEXT("ring must be a machine.USBDevice.Ring or an audiopump.Ring"));
    }
    int slot = usbif_stream_open(dev_id, ep, &cfg);
    if (slot < 0) {
        if (slot == -MP_EACCES) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("claim the endpoint's interface at its streaming alternate setting first"));
        }
        if (slot == -MP_ENOMEM) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("no free stream, or no memory for its transfers"));
        }
        mp_raise_OSError(-slot);
    }
    usbif_stream_release_owners();
    MP_STATE_VM(usbif_stream_owner)[slot] = ring;
    return mp_const_none;
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_KW(usbif_host_stream_obj, 3, usbif_host_stream_py);

// host_stream_stats(dev_id, ep): (packets, bytes, dropped, starved, errors,
// empty, frames, torn, big), or None when ep has no stream.
static mp_obj_t usbif_host_stream_stats_py(mp_obj_t dev_in, mp_obj_t ep_in) {
    #if USBIF_HAVE_HOST
    usbif_stream_stats_t st;
    if (usbif_stream_stats((uint32_t)mp_obj_get_int(dev_in), (uint8_t)mp_obj_get_int(ep_in), &st) < 0) {
        return mp_const_none;
    }
    const uint32_t v[9] = { st.packets, st.bytes, st.dropped, st.starved, st.errors, st.empty,
                            st.frames, st.torn, st.big };
    mp_obj_t items[9];
    for (int i = 0; i < 9; i++) {
        items[i] = mp_obj_new_int_from_uint(v[i]);
    }
    return mp_obj_new_tuple(9, items);
    #else
    (void)dev_in;
    (void)ep_in;
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_stream_stats_obj, usbif_host_stream_stats_py);

// host_stream_write(dev_id, ep, buf): queue PCM on an OUT stream; the bytes
// taken, whole frames, 0 when the ring is full. Through the same gate as a
// C sink's writes, so the two never race on the ring.
static mp_obj_t usbif_host_stream_write_py(mp_obj_t dev_in, mp_obj_t ep_in, mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    return usbif_xfer_check(usbif_stream_write((uint32_t)mp_obj_get_int(dev_in),
        (uint8_t)mp_obj_get_int(ep_in), buf.buf, buf.len));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_3(usbif_host_stream_write_obj, usbif_host_stream_write_py);

// host_stream_space(dev_id, ep): bytes a write can take now, whole frames.
static mp_obj_t usbif_host_stream_space_py(mp_obj_t dev_in, mp_obj_t ep_in) {
    #if USBIF_HAVE_HOST
    return usbif_xfer_check(usbif_stream_space((uint32_t)mp_obj_get_int(dev_in), (uint8_t)mp_obj_get_int(ep_in)));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_stream_space_obj, usbif_host_stream_space_py);

// host_stream_c_sink(dev_id, ep, channels, bits) -> bytes: the OUT stream's
// pcm_c_sink_t by value (pcm_c_sink.h), for another native module to copy
// and write to from its own task. Dead (-1 from both calls) once the stream
// stops; a new stream issues a new one.
static mp_obj_t usbif_host_stream_c_sink_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    pcm_c_sink_t sink;
    int r = usbif_stream_c_sink((uint32_t)mp_obj_get_int(args[0]), (uint8_t)mp_obj_get_int(args[1]), &sink,
        (uint32_t)mp_obj_get_int(args[2]), (uint32_t)mp_obj_get_int(args[3]));
    if (r < 0) {
        mp_raise_OSError(-r);
    }
    return mp_obj_new_bytes((const byte *)&sink, sizeof(sink));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_stream_c_sink_obj, 4, 4, usbif_host_stream_c_sink_py);

// A hosted network adapter (usb.host.net) as an ESP-IDF network interface,
// in usbif_host_net.c: Python does the control side, this moves the frames.
#if USBIF_HAVE_HOST
extern int usbif_net_start(uint32_t dev_id, uint8_t itf, uint8_t ep_in, uint8_t ep_out, uint16_t mps,
    const uint8_t mac[6], bool rndis);
extern void usbif_net_stop(void);
extern int usbif_net_info(uint32_t addr[4], uint32_t counts[5]);
extern void usbif_net_set_default(void);
#endif

// host_net_start(dev_id, interface, ep_in, ep_out, max_packet, mac, rndis)
static mp_obj_t usbif_host_net_start_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t mac;
    mp_get_buffer_raise(args[5], &mac, MP_BUFFER_READ);
    if (mac.len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("a MAC address is 6 bytes"));
    }
    usbif_xfer_check(usbif_net_start((uint32_t)mp_obj_get_int(args[0]), (uint8_t)mp_obj_get_int(args[1]),
        (uint8_t)mp_obj_get_int(args[2]), (uint8_t)mp_obj_get_int(args[3]), (uint16_t)mp_obj_get_int(args[4]),
        mac.buf, mp_obj_is_true(args[6])));
    return mp_const_none;
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_net_start_obj, 7, 7, usbif_host_net_start_py);

static mp_obj_t usbif_host_net_stop_py(void) {
    #if USBIF_HAVE_HOST
    usbif_net_stop();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_net_stop_obj, usbif_host_net_stop_py);

#if USBIF_HAVE_HOST
static mp_obj_t usbif_ip_str(uint32_t a) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
        (unsigned)((a >> 16) & 0xff), (unsigned)(a >> 24));
    return mp_obj_new_str(buf, strlen(buf));
}
#endif

// host_net_info(): (up, (ip, netmask, gateway, dns), (rx, tx, rx dropped,
// tx dropped, rx errors)), or None when no interface is up.
static mp_obj_t usbif_host_net_info_py(void) {
    #if USBIF_HAVE_HOST
    uint32_t addr[4], counts[5];
    int up = usbif_net_info(addr, counts);
    if (up < 0) {
        return mp_const_none;
    }
    mp_obj_t ips[4], cs[5];
    for (int i = 0; i < 4; i++) {
        ips[i] = usbif_ip_str(addr[i]);
    }
    for (int i = 0; i < 5; i++) {
        cs[i] = mp_obj_new_int_from_uint(counts[i]);
    }
    mp_obj_t items[3] = { mp_obj_new_bool(up > 0), mp_obj_new_tuple(4, ips), mp_obj_new_tuple(5, cs) };
    return mp_obj_new_tuple(3, items);
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_net_info_obj, usbif_host_net_info_py);

static mp_obj_t usbif_host_net_default_py(void) {
    #if USBIF_HAVE_HOST
    usbif_net_set_default();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_net_default_obj, usbif_host_net_default_py);

// _usbif.__revision__: which usbif this firmware was built from. The build
// passes USBIF_REVISION (git describe) as a compile definition; a build with no
// git says "unknown" rather than guessing.
#ifndef USBIF_REVISION
#define USBIF_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(_usbif_revision_obj, USBIF_REVISION);

static const mp_rom_map_elem_t usbif_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&_usbif_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__usbif) },
    { MP_ROM_QSTR(MP_QSTR_host_start), MP_ROM_PTR(&usbif_host_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stop), MP_ROM_PTR(&usbif_host_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_devices), MP_ROM_PTR(&usbif_host_devices_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_desc), MP_ROM_PTR(&usbif_host_desc_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_claim), MP_ROM_PTR(&usbif_host_claim_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_release), MP_ROM_PTR(&usbif_host_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_ctrl), MP_ROM_PTR(&usbif_host_ctrl_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_xfer), MP_ROM_PTR(&usbif_host_xfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stream), MP_ROM_PTR(&usbif_host_stream_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stream_stats), MP_ROM_PTR(&usbif_host_stream_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stream_write), MP_ROM_PTR(&usbif_host_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stream_space), MP_ROM_PTR(&usbif_host_stream_space_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stream_c_sink), MP_ROM_PTR(&usbif_host_stream_c_sink_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_net_start), MP_ROM_PTR(&usbif_host_net_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_net_stop), MP_ROM_PTR(&usbif_host_net_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_net_info), MP_ROM_PTR(&usbif_host_net_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_net_default), MP_ROM_PTR(&usbif_host_net_default_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_drain), MP_ROM_PTR(&usbif_host_drain_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stats), MP_ROM_PTR(&usbif_host_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_port_cycle), MP_ROM_PTR(&usbif_host_port_cycle_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_intr_dump), MP_ROM_PTR(&usbif_host_intr_dump_obj) },
};
static MP_DEFINE_CONST_DICT(usbif_module_globals, usbif_module_globals_table);


// --- Soft reset -------------------------------------------------------------
//
// A soft reset wipes the Python heap and leaves C running. Before this, the
// host task, the I2S pump and an attached drive all outlived the program that
// started them: the next host_start() was a no-op on the old host and queued
// no attach events, a stream the old program left open couldn't be opened
// again, host_stop() then wedged the host until a hard reset, uac_pump_start()
// refused because the old pump still held I2S, and the drive went on serving
// a buffer out of a heap that had been reused.
//
// The esp32 port's soft-reset path calls a fixed list of deinit functions
// that a user C module can't join, and MP_REGISTER_MODULE_DELEGATION is for
// attribute lookup, not teardown. But the same path calls gc_sweep_all()
// first, which runs __del__ on every object in the heap that has one,
// reachable or not. That's how machine.I2S, machine.Timer and sockets shut
// down on a soft reset, and it's what usbif uses: a small guard object with a
// finaliser, held in a root pointer so an ordinary gc.collect() never
// finalises it, made the first time a program starts something. Finalisers
// run before the heap's blocks are freed, so the drive's buffer is still
// intact while the drive detaches.

static void usbif_soft_reset_teardown(void) {
    #if USBIF_HAVE_HOST
    // Closes whatever class drivers the program left open, then stops the
    // host task and uninstalls the host library: host_start() starts fresh.
    usbif_host_stop_c();
    usbif_stream_release_owners();
    #endif
    usbif_host_running = false;
    // The guard is being swept with the rest of the heap. Its root has to go
    // too, or the next program's first start would find it still set and
    // never make a live guard, leaving the soft reset after that with nothing
    // to finalise.
    MP_STATE_VM(usbif_soft_reset_guard) = MP_OBJ_NULL;
}

static mp_obj_t usbif_soft_reset_guard_del(mp_obj_t self_in) {
    (void)self_in;
    usbif_soft_reset_teardown();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_soft_reset_guard_del_obj, usbif_soft_reset_guard_del);

static const mp_rom_map_elem_t usbif_soft_reset_guard_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&usbif_soft_reset_guard_del_obj) },
};
static MP_DEFINE_CONST_DICT(usbif_soft_reset_guard_locals, usbif_soft_reset_guard_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    usbif_soft_reset_guard_type,
    MP_QSTR_SoftResetGuard,
    MP_TYPE_FLAG_NONE,
    locals_dict, &usbif_soft_reset_guard_locals
    );

MP_REGISTER_ROOT_POINTER(mp_obj_t usbif_soft_reset_guard);

static void usbif_arm_soft_reset(void) {
    if (MP_STATE_VM(usbif_soft_reset_guard) == MP_OBJ_NULL) {
        mp_obj_base_t *guard = mp_obj_malloc_with_finaliser(mp_obj_base_t,
            &usbif_soft_reset_guard_type);
        MP_STATE_VM(usbif_soft_reset_guard) = MP_OBJ_FROM_PTR(guard);
    }
}

const mp_obj_module_t usbif_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usbif_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__usbif, usbif_user_cmodule);
