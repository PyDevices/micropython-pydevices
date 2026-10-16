// SPDX-License-Identifier: MIT
//
// The `_usbif` native module: the MicroPython-facing surface of usbif.
//
// Scope note (Phase 1): this wires up the module, the capability report and
// the event transport. The IDF USB Host Library and its class drivers arrive
// in Phase 2 and push into the same ring declared here, so nothing about the
// Python-facing contract changes when they do -- which is the point of landing
// the transport first.
//
// The Python half of this backend lives in pydevices `lib/usbif/native_usb.py`
// and documents the same surface from the caller's side. The two are meant to
// be read together.

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"
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
extern int usbif_cdc_open(uint32_t dev_id);
extern int usbif_cdc_write(const uint8_t *data, size_t len);
extern int usbif_cdc_read(uint8_t *out, size_t max);
extern void usbif_cdc_close(void);
extern uint32_t usbif_cdc_rx_dropped(void);
extern int usbif_hid_open(uint32_t dev_id);
extern int usbif_hid_read(uint8_t *out, size_t max);
extern void usbif_hid_close(void);
extern int usbif_msc_open(uint32_t dev_id);
extern int usbif_msc_info(uint32_t *num_blocks, uint32_t *block_size, const char **inquiry);
extern int usbif_msc_read_block(uint32_t lba, uint8_t *out, size_t max);
extern int usbif_msc_write_block(uint32_t lba, const uint8_t *data, size_t len);
extern int usbif_msc_provoke_error(void);
extern int usbif_host_desc_get(uint32_t dev_id, const uint8_t **out, uint16_t *len);
extern int usbif_host_uac_clock_ranges(uint32_t dev_id, uint8_t control_itf, uint8_t clock_id,
    uint32_t *out, int max_triplets);
extern int usbif_host_uac_open(uint32_t dev_id, uint8_t itf, uint8_t alt, uint8_t ep,
    uint16_t mps, uint32_t rate, uint8_t clock, uint8_t control, uint16_t frame,
    uint32_t ring_bytes);
extern int usbif_host_uac_read(uint8_t *out, size_t max);
extern int usbif_host_uac_write(const uint8_t *data, size_t len);
extern int usbif_host_uac_queued(int dir);
extern int usbif_host_uac_space(int dir);
extern int usbif_host_uac_capacity(int dir);
extern int usbif_host_uac_c_sink(pcm_c_sink_t *out, uint32_t channels, uint32_t bits);
extern void usbif_host_uac_stats(int dir, uint32_t *packets, uint32_t *bytes, uint32_t *dropped,
    uint32_t *starved, uint32_t *errors, uint32_t *empty);
extern void usbif_host_uac_close(int dir);
extern int usbif_host_uvc_negotiate(uint32_t dev_id, uint8_t itf, uint8_t format_index,
    uint8_t frame_index, uint32_t interval, uint32_t *payload_out, uint32_t *frame_out);
extern int usbif_host_uvc_open(uint32_t dev_id, uint8_t itf, uint8_t alt, uint8_t ep,
    uint16_t packet, uint32_t frame_bytes);
extern int usbif_host_uvc_read_frame(uint8_t *out, size_t max);
extern int usbif_host_uvc_frame_ready(void);
extern void usbif_host_uvc_stats(uint32_t *frames, uint32_t *packets, uint32_t *bytes,
    uint32_t *dropped_full, uint32_t *dropped_torn, uint32_t *dropped_big,
    uint32_t *errors, uint32_t *empty);
extern void usbif_host_uvc_close(void);
extern int usbif_host_midi_open(uint32_t dev_id);
extern int usbif_host_midi_read(uint8_t *out, size_t max);
extern int usbif_host_midi_write(const uint8_t *data, size_t len);
extern void usbif_host_midi_close(void);
extern uint32_t usbif_host_midi_rx_dropped(void);
extern uint8_t usbif_host_midi_release_failed;
extern uint8_t usbif_msc_last_fail_stage, usbif_msc_last_csw_status, usbif_msc_last_xfer_status;
extern uint8_t usbif_msc_last_sense_key, usbif_msc_last_asc, usbif_msc_last_ascq;
extern int usbif_msc_last_moved;
extern void usbif_msc_close(void);
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

// Classes this firmware was actually built with. Phase 1 left this at zero
// pending class drivers; CDC, HID and MSC host drivers now exist (unconditionally,
// under the same USBIF_HAVE_HOST gate), so an honest answer names those three --
// MIDI joined them (usbif_host_midi.c) once it became clear nobody upstream
// supplies one, UAC joined them with usbif_host_uac.c, and UVC with
// usbif_host_uvc.c. capabilities() reports what can actually be driven, not
// what the enumerator can name -- so a bit appears here only once a driver
// behind it exists.
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
// per the native_usb.py contract) to a bitmask. Unknown names are ignored
// rather than raising -- host_start() intersects the result against
// usbif_supported_classes() anyway, so an unrecognised or unsupported name
// simply doesn't start, which is what "not built into this firmware" should
// look like on the host side too (see NativeDevice.functions() for the same
// philosophy on the device side).
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

static mp_obj_t usbif_capabilities(void) {
    return usbif_classes_to_set(usbif_supported_classes());
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_capabilities_obj, usbif_capabilities);

// Build the DeviceInfo row the Python side turns into a namedtuple. Field
// order matches usbif.DeviceInfo exactly; the parity harness asserts on those
// names, so a change here is a change to the portable contract.
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
    // return value ("the set actually started") that native_usb.py documents.
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

static mp_obj_t usbif_host_stop(void) {
    #if USBIF_HAVE_HOST
    usbif_host_stop_c();
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

// CDC-ACM session: open by the dev_id an attach event (or host_devices row)
// reported. One session at a time in this first cut.
static mp_obj_t usbif_host_cdc_open(mp_obj_t dev_id_in) {
    #if USBIF_HAVE_HOST
    int err = usbif_cdc_open((uint32_t)mp_obj_get_int(dev_id_in));
    if (err != 0) {
        mp_raise_OSError(MP_EIO);
    }
    #else
    (void)dev_id_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_cdc_open_obj, usbif_host_cdc_open);

static mp_obj_t usbif_host_cdc_write(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    // Write the whole buffer through the single in-flight transfer.
    size_t done = 0;
    while (done < buf.len) {
        int n = usbif_cdc_write((const uint8_t *)buf.buf + done, buf.len - done);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    return mp_obj_new_int_from_uint(done);
    #else
    (void)buf_in;
    return MP_OBJ_NEW_SMALL_INT(0);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_cdc_write_obj, usbif_host_cdc_write);

static mp_obj_t usbif_host_cdc_read(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_cdc_read((uint8_t *)buf.buf, buf.len);
    return MP_OBJ_NEW_SMALL_INT(n < 0 ? 0 : n);
    #else
    (void)buf_in;
    return MP_OBJ_NEW_SMALL_INT(0);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_cdc_read_obj, usbif_host_cdc_read);

static mp_obj_t usbif_host_cdc_close_py(void) {
    #if USBIF_HAVE_HOST
    usbif_cdc_close();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_cdc_close_obj, usbif_host_cdc_close_py);

static mp_obj_t usbif_host_hid_open(mp_obj_t dev_id_in) {
    #if USBIF_HAVE_HOST
    if (usbif_hid_open((uint32_t)mp_obj_get_int(dev_id_in)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    #else
    (void)dev_id_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_hid_open_obj, usbif_host_hid_open);

// Returns one input report's length (bytes written into buf), 0 if none.
static mp_obj_t usbif_host_hid_read(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_hid_read((uint8_t *)buf.buf, buf.len);
    return MP_OBJ_NEW_SMALL_INT(n < 0 ? 0 : n);
    #else
    (void)buf_in;
    return MP_OBJ_NEW_SMALL_INT(0);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_hid_read_obj, usbif_host_hid_read);

static mp_obj_t usbif_host_hid_close_py(void) {
    #if USBIF_HAVE_HOST
    usbif_hid_close();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_hid_close_obj, usbif_host_hid_close_py);

static mp_obj_t usbif_host_msc_open(mp_obj_t dev_id_in) {
    #if USBIF_HAVE_HOST
    if (usbif_msc_open((uint32_t)mp_obj_get_int(dev_id_in)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    #else
    (void)dev_id_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_msc_open_obj, usbif_host_msc_open);

// (num_blocks, block_size, inquiry_string)
static mp_obj_t usbif_host_msc_info(void) {
    #if USBIF_HAVE_HOST
    uint32_t blocks, bs;
    const char *inq;
    if (usbif_msc_info(&blocks, &bs, &inq) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    mp_obj_t items[3] = {
        mp_obj_new_int_from_uint(blocks),
        mp_obj_new_int_from_uint(bs),
        mp_obj_new_str(inq, strlen(inq)),
    };
    return mp_obj_new_tuple(3, items);
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_msc_info_obj, usbif_host_msc_info);

static mp_obj_t usbif_host_msc_read(mp_obj_t lba_in, mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_msc_read_block((uint32_t)mp_obj_get_int(lba_in), (uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)lba_in;
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_msc_read_obj, usbif_host_msc_read);

// One block, host-to-device. The buffer must be exactly one block: a short
// write is refused rather than padded, since padding would put whatever
// follows the caller's data into the rest of the sector.
static mp_obj_t usbif_host_msc_write(mp_obj_t lba_in, mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    int n = usbif_msc_write_block((uint32_t)mp_obj_get_int(lba_in),
        (const uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)lba_in;
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_msc_write_obj, usbif_host_msc_write);

// (fail_stage, csw_status, bytes_moved) for the last BOT transaction.
// fail_stage: 0 none, 1 CBW, 2 data, 3 CSW read, 4 CSW malformed, 5 CSW
// reported failure. Distinguishes a transfer that errored from a device
// that rejected the command -- guessing between those wastes a build.
static mp_obj_t usbif_host_msc_diag(void) {
    #if USBIF_HAVE_HOST
    mp_obj_t items[7] = {
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_fail_stage),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_csw_status),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_moved),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_xfer_status),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_sense_key),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_asc),
        MP_OBJ_NEW_SMALL_INT(usbif_msc_last_ascq),
    };
    return mp_obj_new_tuple(7, items);
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_msc_diag_obj, usbif_host_msc_diag);

// Provoke a device-side command failure on purpose, so the recovery path
// and REQUEST SENSE can be proven on real hardware instead of assumed.
// Returns the transaction result (negative); read host_msc_diag() after.
static mp_obj_t usbif_host_msc_provoke(void) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_msc_provoke_error());
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_msc_provoke_obj, usbif_host_msc_provoke);

// --- MIDI host -------------------------------------------------------
// Deliberately the same shape as the device-side midi_read/midi_write: a
// plain MIDI byte stream both ways, so an application that harmonises or
// logs does not care which end of the cable it is on. The USB-MIDI packet
// framing lives in usbif_host_midi.c and never reaches Python.

// The raw active configuration descriptor of a hosted device.
//
// Everything a class needs to describe itself is in here -- interfaces, alt
// settings, endpoints, and the class-specific descriptors that carry audio
// formats and video frame tables. Returned whole so the parsing happens in
// Python, where a wrong guess costs a re-run rather than a reflash.
// --- UAC host ---------------------------------------------------------
// The stream to open is chosen in Python by usbif.uac, which reads the whole
// descriptor and picks an alternate setting; these arguments are that answer.
// Deliberately not rediscovered here: a UAC device can offer two dozen
// formats, and choosing among them is configuration, not byte movement.
static mp_obj_t usbif_host_uac_open_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    int rc = usbif_host_uac_open(
        (uint32_t)mp_obj_get_int(args[0]),
        (uint8_t)mp_obj_get_int(args[1]),
        (uint8_t)mp_obj_get_int(args[2]),
        (uint8_t)mp_obj_get_int(args[3]),
        (uint16_t)mp_obj_get_int(args[4]),
        n_args > 5 ? (uint32_t)mp_obj_get_int(args[5]) : 0,
        n_args > 6 ? (uint8_t)mp_obj_get_int(args[6]) : 0,    // 2.0 clock source id
        n_args > 7 ? (uint8_t)mp_obj_get_int(args[7]) : 0,    // its AudioControl interface
        n_args > 8 ? (uint16_t)mp_obj_get_int(args[8]) : 0,   // bytes per audio frame
        n_args > 9 ? (uint32_t)mp_obj_get_int(args[9]) : 0);  // ring bytes, 0 = 8 KB
    if (rc != 0) {
        // Carry the driver's own code: -1 a stream in this direction is
        // already open, -2 bad packet size, -3 no such device, -4 alt 0
        // carries no endpoint, -5 claim refused, -6 transfer alloc failed,
        // -7 nothing submitted, -8 no memory for the ring, -9 ring size out
        // of range (two packets to 4 MB), -10 the other direction's stream
        // already holds this interface.
        if (rc == -5) {
            // IDF answers ESP_ERR_NOT_SUPPORTED to a claim whose endpoint
            // packet exceeds the FIFO the host was installed with, and says
            // nothing about packets; the usual reason on this bench.
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("host_uac_open failed (-5): interface claim refused -- "
                              "usually the stream's packet exceeds the host FIFO split; "
                              "start the host for 'uac' without 'uvc'"));
        }
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_uac_open failed (%d)"), rc);
    }
    return mp_const_none;
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_open_obj, 5, 10, usbif_host_uac_open_py);

// The calls below act on one of the two streams the driver keeps, one per
// direction. Each takes the stream's endpoint address, optionally: bit 7
// picks it, as on the wire. Without one they mean what they meant when there
// was a single stream: whichever is open, playback first; close() closes both.
// read() and write() need no address: only capture reads, only playback writes.
#if USBIF_HAVE_HOST
static int usbif_host_uac_dir(size_t n_args, const mp_obj_t *args) {
    if (n_args == 0 || args[0] == mp_const_none) {
        return USBIF_HOST_UAC_ANY;
    }
    return (mp_obj_get_int(args[0]) & 0x80) ? USBIF_HOST_UAC_IN : USBIF_HOST_UAC_OUT;
}
#endif

static mp_obj_t usbif_host_uac_read_py(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_host_uac_read((uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_uac_read_obj, usbif_host_uac_read_py);

static mp_obj_t usbif_host_uac_write_py(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    int n = usbif_host_uac_write((const uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_uac_write_obj, usbif_host_uac_write_py);

static mp_obj_t usbif_host_uac_queued_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_host_uac_queued(usbif_host_uac_dir(n_args, args)));
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_queued_obj, 0, 1, usbif_host_uac_queued_py);

// Free room and total size of the stream's ring, in bytes; -1 when no stream
// is open. A caller that must not block asks space() before it writes
// (usbif#36), rather than hardcoding a size the ring no longer has.
static mp_obj_t usbif_host_uac_space_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_host_uac_space(usbif_host_uac_dir(n_args, args)));
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_space_obj, 0, 1, usbif_host_uac_space_py);

static mp_obj_t usbif_host_uac_capacity_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_host_uac_capacity(usbif_host_uac_dir(n_args, args)));
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_capacity_obj, 0, 1, usbif_host_uac_capacity_py);

// host_uac_c_sink(channels, bits) -> bytes: the open playback stream's
// pcm_c_sink_t, by value (usbif#43). Another native module copies it with
// pcm_c_sink_copy() and writes to the stream from its own task. By value so
// nothing a usermod keeps points into this heap; the sink's lifetime is the
// stream's, enforced in C (shared/usbif_pcm_sink.h). OSError(EIO) when no
// playback stream is open.
static mp_obj_t usbif_host_uac_c_sink_py(mp_obj_t channels_in, mp_obj_t bits_in) {
    #if USBIF_HAVE_HOST
    pcm_c_sink_t sink;
    if (usbif_host_uac_c_sink(&sink, (uint32_t)mp_obj_get_int(channels_in),
        (uint32_t)mp_obj_get_int(bits_in)) != 0) {
        mp_raise_OSError(MP_EIO);
    }
    return mp_obj_new_bytes((const byte *)&sink, sizeof(sink));
    #else
    (void)channels_in; (void)bits_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_2(usbif_host_uac_c_sink_obj, usbif_host_uac_c_sink_py);

// (packets, bytes, dropped, starved, errors). When a stream sounds wrong the
// first question is whether bytes are being lost and where, and these
// separate the three answers: the ring overflowed because Python was late
// (dropped), the ring was empty when the bus asked (starved), or the bus
// itself reported a bad packet (errors).
static mp_obj_t usbif_host_uac_stats_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    uint32_t packets = 0, bytes = 0, dropped = 0, starved = 0, errors = 0, empty = 0;
    usbif_host_uac_stats(usbif_host_uac_dir(n_args, args),
        &packets, &bytes, &dropped, &starved, &errors, &empty);
    mp_obj_t items[6] = {
        mp_obj_new_int_from_uint(packets),
        mp_obj_new_int_from_uint(bytes),
        mp_obj_new_int_from_uint(dropped),
        mp_obj_new_int_from_uint(starved),
        mp_obj_new_int_from_uint(errors),
        mp_obj_new_int_from_uint(empty),
    };
    return mp_obj_new_tuple(6, items);
    #else
    (void)n_args; (void)args;
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_stats_obj, 0, 1, usbif_host_uac_stats_py);

// (dev_id, control_itf, clock_id) -> a flat tuple of (min, max, res) triplets
// from a USB Audio 2.0 clock source; usbif.uac.rates_from_ranges() reads it.
static mp_obj_t usbif_host_uac_clock_ranges_py(mp_obj_t dev_in, mp_obj_t itf_in, mp_obj_t clock_in) {
    #if USBIF_HAVE_HOST
    uint32_t triplets[3 * 8];
    int n = usbif_host_uac_clock_ranges((uint32_t)mp_obj_get_int(dev_in),
        (uint8_t)mp_obj_get_int(itf_in), (uint8_t)mp_obj_get_int(clock_in), triplets, 8);
    if (n < 0) {
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_uac_clock_ranges failed (%d): -1 no such device, -2 request refused, -3 short answer"), n);
    }
    mp_obj_t items[3 * 8];
    for (int i = 0; i < 3 * n; i++) {
        items[i] = mp_obj_new_int_from_uint(triplets[i]);
    }
    return mp_obj_new_tuple((size_t)(3 * n), items);
    #else
    (void)dev_in; (void)itf_in; (void)clock_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_3(usbif_host_uac_clock_ranges_obj, usbif_host_uac_clock_ranges_py);

static mp_obj_t usbif_host_uac_close_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    usbif_host_uac_close(usbif_host_uac_dir(n_args, args));
    #else
    (void)n_args; (void)args;
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uac_close_obj, 0, 1, usbif_host_uac_close_py);

// --- UVC host -----------------------------------------------------------

// Negotiate a mode, and return what the device agreed to as
// (payload_bytes, frame_bytes). Both numbers come from the camera, not from
// us: payload_bytes selects the alternate setting and frame_bytes sizes the
// assembly buffers, and a host that guesses either gets torn frames.
static mp_obj_t usbif_host_uvc_negotiate_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    uint32_t payload = 0, frame = 0;
    int rc = usbif_host_uvc_negotiate(
        (uint32_t)mp_obj_get_int(args[0]),
        (uint8_t)mp_obj_get_int(args[1]),
        (uint8_t)mp_obj_get_int(args[2]),
        (uint8_t)mp_obj_get_int(args[3]),
        n_args > 4 ? (uint32_t)mp_obj_get_int(args[4]) : 0,
        &payload, &frame);
    if (rc != 0) {
        // -1 no such device, -2 PROBE set refused, -3 PROBE read refused,
        // -4 COMMIT refused.
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_uvc_negotiate failed (%d)"), rc);
    }
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(payload),
        mp_obj_new_int_from_uint(frame),
    };
    return mp_obj_new_tuple(2, items);
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uvc_negotiate_obj, 4, 5, usbif_host_uvc_negotiate_py);

static mp_obj_t usbif_host_uvc_open_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    (void)n_args;
    int rc = usbif_host_uvc_open(
        (uint32_t)mp_obj_get_int(args[0]),
        (uint8_t)mp_obj_get_int(args[1]),
        (uint8_t)mp_obj_get_int(args[2]),
        (uint8_t)mp_obj_get_int(args[3]),
        (uint16_t)mp_obj_get_int(args[4]),
        (uint32_t)mp_obj_get_int(args[5]));
    if (rc != 0) {
        // -1 already open, -2 bad packet size or alt 0, -3 no such device,
        // -4 frame buffers would not allocate, -5 claim refused,
        // -6 transfer alloc failed, -7 nothing submitted.
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_uvc_open failed (%d)"), rc);
    }
    return mp_const_none;
    #else
    (void)n_args; (void)args;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_uvc_open_obj, 6, 6, usbif_host_uvc_open_py);

// One whole frame or nothing. A partial frame is never returned: the caller
// cannot tell a truncated JPEG from a complete one, so it is not offered.
// Raises ENOBUFS rather than truncating if the buffer is too small, and
// leaves the frame in place to be retried.
static mp_obj_t usbif_host_uvc_read_frame_py(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_host_uvc_read_frame((uint8_t *)buf.buf, buf.len);
    if (n == -2) {
        mp_raise_OSError(MP_ENOBUFS);
    }
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_uvc_read_frame_obj, usbif_host_uvc_read_frame_py);

// Bytes in the waiting frame, or 0. Lets a caller size a buffer before asking
// for the frame rather than guessing and catching EMSGSIZE.
static mp_obj_t usbif_host_uvc_frame_ready_py(void) {
    #if USBIF_HAVE_HOST
    return MP_OBJ_NEW_SMALL_INT(usbif_host_uvc_frame_ready());
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_uvc_frame_ready_obj, usbif_host_uvc_frame_ready_py);

// (frames, packets, bytes, dropped_full, dropped_torn, dropped_big, errors,
// empty). Three separate drop causes because they call for three different
// fixes: the consumer is too slow (full), the bus lost a packet mid-frame
// (torn), or the frame buffer was sized smaller than the camera's frames
// (big) -- which is our mistake, not the bus's, and would otherwise hide
// inside a single "dropped" number.
static mp_obj_t usbif_host_uvc_stats_py(void) {
    #if USBIF_HAVE_HOST
    uint32_t frames = 0, packets = 0, bytes = 0, full = 0, torn = 0, big = 0;
    uint32_t errors = 0, empty = 0;
    usbif_host_uvc_stats(&frames, &packets, &bytes, &full, &torn, &big,
        &errors, &empty);
    mp_obj_t items[8] = {
        mp_obj_new_int_from_uint(frames),
        mp_obj_new_int_from_uint(packets),
        mp_obj_new_int_from_uint(bytes),
        mp_obj_new_int_from_uint(full),
        mp_obj_new_int_from_uint(torn),
        mp_obj_new_int_from_uint(big),
        mp_obj_new_int_from_uint(errors),
        mp_obj_new_int_from_uint(empty),
    };
    return mp_obj_new_tuple(8, items);
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_uvc_stats_obj, usbif_host_uvc_stats_py);

static mp_obj_t usbif_host_uvc_close_py(void) {
    #if USBIF_HAVE_HOST
    usbif_host_uvc_close();
    return mp_const_none;
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_uvc_close_obj, usbif_host_uvc_close_py);

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
extern int usbif_xfer_ep(uint32_t dev_id, uint8_t ep, uint8_t *data, size_t len, uint32_t timeout_ms);

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

// host_xfer(dev_id, endpoint, buf, timeout_ms): a bulk or interrupt
// transfer; an IN endpoint reads into buf, an OUT one sends it. Returns the
// bytes moved. timeout_ms 0 waits for good.
static mp_obj_t usbif_host_xfer_py(size_t n_args, const mp_obj_t *args) {
    #if USBIF_HAVE_HOST
    uint8_t ep = (uint8_t)mp_obj_get_int(args[1]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[2], &buf, (ep & 0x80) ? MP_BUFFER_WRITE : MP_BUFFER_READ);
    return usbif_xfer_check(usbif_xfer_ep((uint32_t)mp_obj_get_int(args[0]), ep, buf.buf, buf.len,
        (uint32_t)mp_obj_get_int(args[3])));
    #else
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbif_host_xfer_obj, 4, 4, usbif_host_xfer_py);

static mp_obj_t usbif_host_midi_open_py(mp_obj_t dev_id_in) {
    #if USBIF_HAVE_HOST
    int rc = usbif_host_midi_open((uint32_t)mp_obj_get_int(dev_id_in));
    if (rc != 0) {
        // Carry the driver's own code: -4 no MIDIStreaming interface, -5 max
        // packet too large, -6 interface claim refused, -7 transfer alloc
        // failed, -8 first submit failed. Collapsing all of those into a bare
        // EIO is what made a concurrency failure unreadable.
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("host_midi_open failed (%d)"), rc);
    }
    return mp_const_none;
    #else
    (void)dev_id_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_midi_open_obj, usbif_host_midi_open_py);

static mp_obj_t usbif_host_midi_read_py(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    int n = usbif_host_midi_read((uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_midi_read_obj, usbif_host_midi_read_py);

// Returns how many bytes were consumed, which can be fewer than offered: a
// trailing partial message is left for the next call rather than sent half
// finished.
static mp_obj_t usbif_host_midi_write_py(mp_obj_t buf_in) {
    #if USBIF_HAVE_HOST
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_READ);
    int n = usbif_host_midi_write((const uint8_t *)buf.buf, buf.len);
    if (n < 0) {
        mp_raise_OSError(MP_EIO);
    }
    return MP_OBJ_NEW_SMALL_INT(n);
    #else
    (void)buf_in;
    mp_raise_OSError(MP_EOPNOTSUPP);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_1(usbif_host_midi_write_obj, usbif_host_midi_write_py);

static mp_obj_t usbif_host_midi_close_py(void) {
    #if USBIF_HAVE_HOST
    usbif_host_midi_close();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_midi_close_obj, usbif_host_midi_close_py);

// (dropped_bytes, release_failed). The second is the one that matters when
// a later host_stop() misbehaves: a stuck interface claim surfaces there,
// far from its cause.
static mp_obj_t usbif_host_midi_dropped_py(void) {
    #if USBIF_HAVE_HOST
    mp_obj_t items[2] = {
        mp_obj_new_int_from_uint(usbif_host_midi_rx_dropped()),
        MP_OBJ_NEW_SMALL_INT(usbif_host_midi_release_failed),
    };
    return mp_obj_new_tuple(2, items);
    #else
    return mp_const_none;
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_midi_dropped_obj, usbif_host_midi_dropped_py);

static mp_obj_t usbif_host_msc_close_py(void) {
    #if USBIF_HAVE_HOST
    usbif_msc_close();
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(usbif_host_msc_close_obj, usbif_host_msc_close_py);


// _usbif.__revision__: which usbif this firmware was built from. The build
// passes USBIF_REVISION (git describe) as a compile definition; a build with no
// git says "unknown" rather than guessing.
#ifndef USBIF_REVISION
#define USBIF_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(_usbif_revision_obj, USBIF_REVISION);

static const mp_rom_map_elem_t usbif_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&_usbif_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_FN_CDC), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_FN_AUDIO), MP_ROM_INT(4) },
    { MP_ROM_QSTR(MP_QSTR_FN_VIDEO), MP_ROM_INT(32) },
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__usbif) },
    { MP_ROM_QSTR(MP_QSTR_capabilities), MP_ROM_PTR(&usbif_capabilities_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_start), MP_ROM_PTR(&usbif_host_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stop), MP_ROM_PTR(&usbif_host_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_devices), MP_ROM_PTR(&usbif_host_devices_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_desc), MP_ROM_PTR(&usbif_host_desc_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_claim), MP_ROM_PTR(&usbif_host_claim_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_release), MP_ROM_PTR(&usbif_host_release_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_ctrl), MP_ROM_PTR(&usbif_host_ctrl_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_xfer), MP_ROM_PTR(&usbif_host_xfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_open), MP_ROM_PTR(&usbif_host_uac_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_read), MP_ROM_PTR(&usbif_host_uac_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_write), MP_ROM_PTR(&usbif_host_uac_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_queued), MP_ROM_PTR(&usbif_host_uac_queued_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_space), MP_ROM_PTR(&usbif_host_uac_space_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_capacity), MP_ROM_PTR(&usbif_host_uac_capacity_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_c_sink), MP_ROM_PTR(&usbif_host_uac_c_sink_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_stats), MP_ROM_PTR(&usbif_host_uac_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_clock_ranges), MP_ROM_PTR(&usbif_host_uac_clock_ranges_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uac_close), MP_ROM_PTR(&usbif_host_uac_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_negotiate), MP_ROM_PTR(&usbif_host_uvc_negotiate_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_open), MP_ROM_PTR(&usbif_host_uvc_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_read_frame), MP_ROM_PTR(&usbif_host_uvc_read_frame_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_frame_ready), MP_ROM_PTR(&usbif_host_uvc_frame_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_stats), MP_ROM_PTR(&usbif_host_uvc_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_uvc_close), MP_ROM_PTR(&usbif_host_uvc_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_drain), MP_ROM_PTR(&usbif_host_drain_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_stats), MP_ROM_PTR(&usbif_host_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_port_cycle), MP_ROM_PTR(&usbif_host_port_cycle_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_intr_dump), MP_ROM_PTR(&usbif_host_intr_dump_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_cdc_open), MP_ROM_PTR(&usbif_host_cdc_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_cdc_write), MP_ROM_PTR(&usbif_host_cdc_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_cdc_read), MP_ROM_PTR(&usbif_host_cdc_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_cdc_close), MP_ROM_PTR(&usbif_host_cdc_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_hid_open), MP_ROM_PTR(&usbif_host_hid_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_hid_read), MP_ROM_PTR(&usbif_host_hid_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_hid_close), MP_ROM_PTR(&usbif_host_hid_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_open), MP_ROM_PTR(&usbif_host_msc_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_info), MP_ROM_PTR(&usbif_host_msc_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_read), MP_ROM_PTR(&usbif_host_msc_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_write), MP_ROM_PTR(&usbif_host_msc_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_diag), MP_ROM_PTR(&usbif_host_msc_diag_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_provoke), MP_ROM_PTR(&usbif_host_msc_provoke_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_midi_open), MP_ROM_PTR(&usbif_host_midi_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_midi_read), MP_ROM_PTR(&usbif_host_midi_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_midi_write), MP_ROM_PTR(&usbif_host_midi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_midi_close), MP_ROM_PTR(&usbif_host_midi_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_midi_dropped), MP_ROM_PTR(&usbif_host_midi_dropped_obj) },
    { MP_ROM_QSTR(MP_QSTR_host_msc_close), MP_ROM_PTR(&usbif_host_msc_close_obj) },
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
