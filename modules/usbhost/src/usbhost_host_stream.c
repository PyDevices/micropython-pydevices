// SPDX-License-Identifier: MIT
//
// Isochronous host streams: a hosted device's isochronous endpoint moved to
// or from a ring in C, for usb.host.audio and usb.host.video. See
// usbhost_host_stream.h for the surface; the class logic is all in Python.
//
// Isochronous is not bulk. A bulk transfer moves a byte stream and retries;
// an isochronous transfer is a schedule -- one packet per bus interval,
// delivered late or not at all, never retried. So each transfer carries many
// packets, several stay in flight, and a gap is counted rather than raised.
// The ring is there so neither side waits: the bus never waits for Python,
// and Python never waits for the bus.
//
// Completions are delivered by whoever pumps the host client's events, under
// the host lock: the host task, or a caller waiting on a transfer of its own
// (usbhost_host_xfer.c). Either way one at a time, so each stream's ring has
// one producer (IN) or one consumer (OUT) on this side, and the interpreter,
// a usermod's task or audiodsp's pump thread on the other.
//
// This replaces usbhost's UAC and UVC host engines (usbhost_host_uac.c,
// usbhost_host_uvc.c), and keeps what they learned: the queue sized in bus time
// against the host task's tick (usbhost_iso_queue.h, usbif#67), playback paced
// to the rate rather than the endpoint's maximum (usbhost_iso_pace.h), packets
// laid out at their requested size, not their actual one, a frame published
// only whole (usbhost_uvc_frame.h), and transfers never freed while the
// library still holds them.

#include "py/mpconfig.h"

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#if defined(CONFIG_SOC_USB_OTG_SUPPORTED) && CONFIG_SOC_USB_OTG_SUPPORTED

#include <string.h>

#include "py/mperrno.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "usbhost_host_stream.h"
#include "shared/usbhost_downmix.h"
#include "shared/usbhost_iso_pace.h"
#include "shared/usbhost_iso_queue.h"
#include "shared/usbhost_pcm_sink.h"
#include "shared/usbhost_uvc_frame.h"

#include "usbhost_host.h"

// The most a packet can be: 1024 bytes times three transactions a
// microframe, a high-speed high-bandwidth endpoint.
#define USBHOST_STREAM_MAX_PACKET (3072)

// How long a stop waits for its transfers to retire on their own (each is a
// few milliseconds of bus time), and then again after halting the endpoint.
#define USBHOST_STREAM_RETIRE_US  (60000)
#define USBHOST_STREAM_HALT_US    (150000)

typedef struct {
    bool used;              // holds a stream, running or retiring
    volatile bool open;     // transfers resubmit while set
    bool gone;              // the device went away under it
    bool in;
    uint32_t dev_id;
    usb_device_handle_t dev;
    uint8_t itf, ep;
    uint16_t packet;
    uint8_t pkts, nxfer;
    usb_transfer_t *xfer[USBHOST_ISO_MAX_XFER];
    volatile uint8_t inflight;
    usbhost_stream_cfg_t cfg;
    bool paced;
    usbhost_iso_pace_t pace;
    usbhost_uvc_frame_t vid;
    volatile usbhost_stream_stats_t st;
} usbhost_stream_t;

static usbhost_stream_t usbhost_streams[USBHOST_HOST_STREAMS];

// The producers' gate on each playback stream's ring: the interpreter's
// writes and a C sink's, from any task (shared/usbhost_pcm_sink.h). Outside
// the slots because a slot is cleared for each stream, and a gate's
// generations must carry on from one stream to the next, so a sink issued
// for a stream that closed stays closed.
static usbhost_pcm_sink_t usbhost_stream_gates[USBHOST_HOST_STREAMS];
static bool usbhost_stream_gates_ready;

static uint32_t usbhost_stream_gate_room(void *slot) {
    const usbhost_stream_t *s = slot;
    uint32_t room = s->cfg.ring.size(s->cfg.ring.ctx) - s->cfg.ring.used(s->cfg.ring.ctx);
    return room;
}

static uint32_t usbhost_stream_gate_push(void *slot, const uint8_t *src, uint32_t n) {
    const usbhost_stream_t *s = slot;
    return s->cfg.ring.put(s->cfg.ring.ctx, src, n);
}

static const usbhost_pcm_ring_ops_t usbhost_stream_gate_ops = {
    usbhost_stream_gate_room, usbhost_stream_gate_push,
};

static void usbhost_stream_gate_wait(void) {
    vTaskDelay(1);      // one real tick: lets a writer on this core finish
}

static void usbhost_stream_gates_setup(void) {
    if (!usbhost_stream_gates_ready) {
        for (int i = 0; i < USBHOST_HOST_STREAMS; i++) {
            usbhost_pcm_sink_init(&usbhost_stream_gates[i], &usbhost_stream_gate_ops, &usbhost_streams[i],
                usbhost_stream_gate_wait);
        }
        usbhost_stream_gates_ready = true;
    }
}

static inline int usbhost_stream_index(const usbhost_stream_t *s) {
    return (int)(s - usbhost_streams);
}

static usbhost_stream_t *usbhost_stream_find(uint32_t dev_id, uint8_t ep) {
    for (int i = 0; i < USBHOST_HOST_STREAMS; i++) {
        usbhost_stream_t *s = &usbhost_streams[i];
        if (s->used && s->dev_id == dev_id && s->ep == ep) {
            return s;
        }
    }
    return NULL;
}

// --- moving packets -------------------------------------------------------

// A playback transfer's packets, from the ring. Each carries the interval's
// worth (paced) or what the ring has; a ring short of a paced packet is
// topped up with silence, because an underrun in audio is silence, not a
// stall, and the stream must keep its slot on the bus. Whole frames only:
// half a frame in a packet padded with zeros would shift every sample after.
static void usbhost_stream_fill(usbhost_stream_t *s, usb_transfer_t *x) {
    const usbhost_stream_ring_t *r = &s->cfg.ring;
    const uint32_t frame = s->cfg.frame ? s->cfg.frame : 1;
    uint32_t total = 0;
    for (int i = 0; i < s->pkts; i++) {
        uint32_t want = s->paced ? usbhost_iso_pace_next(&s->pace, s->packet) : s->packet;
        uint32_t have = r->used(r->ctx);
        uint32_t take = have < want ? have : want;
        take -= take % frame;
        if (!s->paced) {
            want = take;
        }
        if (take) {
            r->get(r->ctx, x->data_buffer + total, take);
        }
        if (take < want) {
            memset(x->data_buffer + total + take, 0, want - take);
            s->st.starved++;
        }
        x->isoc_packet_desc[i].num_bytes = (int)want;
        total += want;
    }
    // IDF refuses a transfer whose num_bytes isn't the sum of its packets'.
    x->num_bytes = (int)total;
}

static void usbhost_stream_arm_in(usbhost_stream_t *s, usb_transfer_t *x) {
    for (int i = 0; i < s->pkts; i++) {
        x->isoc_packet_desc[i].num_bytes = s->packet;
    }
    x->num_bytes = (int)s->packet * s->pkts;
}

static void usbhost_stream_cb(usb_transfer_t *x) {
    usbhost_stream_t *s = (usbhost_stream_t *)x->context;
    if (!s->open) {
        s->inflight--;
        return;
    }
    if (s->in) {
        uint32_t offset = 0;
        for (int i = 0; i < x->num_isoc_packets; i++) {
            usb_isoc_packet_desc_t *pkt = &x->isoc_packet_desc[i];
            uint8_t *data = x->data_buffer + offset;
            const uint32_t n = (uint32_t)pkt->actual_num_bytes;
            if (pkt->status == USB_TRANSFER_STATUS_COMPLETED && n) {
                s->st.packets++;
                s->st.bytes += n;
                if (s->cfg.video) {
                    usbhost_uvc_frame_payload(&s->vid, data, n);
                } else {
                    // The packet is ours until it's resubmitted, so a
                    // downmix can rewrite it where it lies.
                    const uint32_t m = s->cfg.downmix ? usbhost_downmix_s16(data, n) : n;
                    if (m && s->cfg.ring.put(s->cfg.ring.ctx, data, m) < m) {
                        s->st.dropped++;
                    }
                }
            } else if (pkt->status != USB_TRANSFER_STATUS_COMPLETED) {
                s->st.errors++;
            } else {
                // Completed carrying nothing: neither data nor an error. A
                // camera between frames sends these; counted, so silence
                // from a device that stopped reads differently from never
                // having started.
                s->st.empty++;
            }
            // The next packet starts where this one's request ended,
            // however few bytes arrived: advancing by the actual count is
            // the classic isochronous read bug.
            offset += s->packet;
        }
        usbhost_stream_arm_in(s, x);
    } else {
        for (int i = 0; i < x->num_isoc_packets; i++) {
            if (x->isoc_packet_desc[i].status == USB_TRANSFER_STATUS_COMPLETED) {
                s->st.packets++;
                s->st.bytes += x->isoc_packet_desc[i].actual_num_bytes;
            } else {
                s->st.errors++;
            }
        }
        usbhost_stream_fill(s, x);
    }
    if (usb_host_transfer_submit(x) != ESP_OK) {
        s->inflight--;
        s->st.errors++;
    }
}

// --- stopping -------------------------------------------------------------

// Pump the client's events until the stream's transfers are back, or the
// time is up. Works from the host task and from an API caller alike: both
// hold the (recursive) host lock, and the pump is what delivers callbacks.
static void usbhost_stream_wait_retired(usbhost_stream_t *s, int64_t limit_us) {
    const int64_t start = esp_timer_get_time();
    while (s->inflight && esp_timer_get_time() - start < limit_us) {
        usb_host_client_handle_events(usbhost_host_client_get(), 0);
        esp_rom_delay_us(100);
    }
}

// With the host lock held. Afterwards the slot is free, unless a transfer
// never came back, in which case it is left allocated rather than freed
// under the library, and the slot with it.
static void usbhost_stream_stop_locked(usbhost_stream_t *s) {
    if (!s->used) {
        return;
    }
    s->open = false;
    if (!s->in) {
        // First the producers: a usermod's task may be inside a write on the
        // other core. This waits it out, so nothing else reaches the ring.
        usbhost_pcm_sink_close(&usbhost_stream_gates[usbhost_stream_index(s)]);
    }
    usbhost_stream_wait_retired(s, USBHOST_STREAM_RETIRE_US);
    if (s->inflight && !s->gone) {
        // Still out: take them back from the endpoint.
        usb_host_endpoint_halt(s->dev, s->ep);
        usb_host_endpoint_flush(s->dev, s->ep);
        usbhost_stream_wait_retired(s, USBHOST_STREAM_HALT_US);
        usb_host_endpoint_clear(s->dev, s->ep);
    } else if (s->inflight) {
        usbhost_stream_wait_retired(s, USBHOST_STREAM_HALT_US);
    }
    if (s->inflight) {
        printf("usbhost_stream: ep 0x%02x left %u transfers in flight; not freed\n",
            (unsigned)s->ep, (unsigned)s->inflight);
        return;
    }
    for (int i = 0; i < USBHOST_ISO_MAX_XFER; i++) {
        if (s->xfer[i] != NULL) {
            usb_host_transfer_free(s->xfer[i]);
            s->xfer[i] = NULL;
        }
    }
    s->used = false;
}

// The streams on one interface of a device (itf < 0: all of it), as the
// transfer layer gives the interface up; device_gone when it's unplugged.
void usbhost_stream_stop_itf(void *dev, int itf, bool device_gone) {
    for (int i = 0; i < USBHOST_HOST_STREAMS; i++) {
        usbhost_stream_t *s = &usbhost_streams[i];
        if (s->used && s->dev == (usb_device_handle_t)dev && (itf < 0 || s->itf == itf)) {
            if (device_gone) {
                s->gone = true;
            }
            usbhost_stream_stop_locked(s);
        }
    }
}

void usbhost_stream_close_all(void) {
    usbhost_host_lock();
    for (int i = 0; i < USBHOST_HOST_STREAMS; i++) {
        usbhost_stream_stop_locked(&usbhost_streams[i]);
    }
    usbhost_host_unlock();
}

uint32_t usbhost_stream_free_slots(void) {
    uint32_t bits = 0;
    for (int i = 0; i < USBHOST_HOST_STREAMS; i++) {
        if (!usbhost_streams[i].used) {
            bits |= 1u << i;
        }
    }
    return bits;
}

// --- the surface ----------------------------------------------------------

static int usbhost_stream_open_locked(uint32_t dev_id, uint8_t ep, const usbhost_stream_cfg_t *cfg) {
    usbhost_stream_gates_setup();
    usb_device_handle_t dev;
    if (usbhost_host_dev_lookup(dev_id, &dev) != 0) {
        return -MP_ENODEV;
    }
    if (usbhost_stream_find(dev_id, ep) != NULL) {
        return -MP_EBUSY;
    }
    usbhost_stream_t *s = NULL;
    for (int i = 0; i < USBHOST_HOST_STREAMS && s == NULL; i++) {
        if (!usbhost_streams[i].used) {
            s = &usbhost_streams[i];
        }
    }
    if (s == NULL) {
        return -MP_ENOMEM;
    }
    const bool in = (ep & 0x80) != 0;
    const usbhost_stream_ring_t *r = &cfg->ring;
    if (in ? r->put == NULL : (r->get == NULL || r->used == NULL || r->size == NULL)) {
        return -MP_EINVAL;
    }
    if (cfg->video && (!in || r->stage == NULL || r->commit == NULL || r->overflow == NULL)) {
        return -MP_EINVAL;
    }
    if (cfg->downmix && (!in || cfg->video)) {
        return -MP_EINVAL;
    }
    uint16_t wmax;
    uint8_t binterval, itf;
    int rc = usbhost_xfer_iso_ep(dev, ep, &wmax, &binterval, &itf);
    if (rc < 0) {
        return rc;
    }
    uint32_t packet = cfg->packet ? cfg->packet : (uint32_t)(wmax & 0x7FF) * (1 + ((wmax >> 11) & 3));
    if (packet == 0 || packet > USBHOST_STREAM_MAX_PACKET) {
        return -MP_EINVAL;
    }
    usb_device_info_t info;
    const bool high_speed = usb_host_device_info(dev, &info) == ESP_OK && info.speed == USB_SPEED_HIGH;
    // An isochronous bInterval is an exponent on either bus: 2^(b-1) slots.
    const uint32_t interval = (binterval >= 1 && binterval <= 16) ? 1u << (binterval - 1) : 1;
    const usbhost_iso_queue_t q = usbhost_iso_queue_size(high_speed, interval, portTICK_PERIOD_MS);

    memset(s, 0, sizeof(*s));
    s->dev_id = dev_id;
    s->dev = dev;
    s->itf = itf;
    s->ep = ep;
    s->in = in;
    s->packet = (uint16_t)packet;
    s->pkts = q.pkts_per_xfer;
    s->cfg = *cfg;
    s->paced = !in && cfg->rate && cfg->frame;
    usbhost_iso_pace_init(&s->pace, cfg->rate, cfg->frame, interval * (high_speed ? 125u : 1000u));
    if (cfg->video) {
        s->vid.ring = r->ctx;
        s->vid.stage = r->stage;
        s->vid.commit = r->commit;
        s->vid.overflow = r->overflow;
        s->vid.max_frame = cfg->max_frame;
        usbhost_uvc_frame_start(&s->vid);
    }
    // Transfers come from DMA-capable internal RAM, the scarce kind: stream
    // with what fits, as long as that's no less than the minimum queue.
    const size_t buf = (size_t)packet * s->pkts;
    for (int i = 0; i < q.num_xfer; i++) {
        if (usb_host_transfer_alloc(buf, s->pkts, &s->xfer[i]) != ESP_OK) {
            s->xfer[i] = NULL;
            break;
        }
        s->nxfer++;
        s->xfer[i]->device_handle = dev;
        s->xfer[i]->bEndpointAddress = ep;
        s->xfer[i]->callback = usbhost_stream_cb;
        s->xfer[i]->context = s;
        s->xfer[i]->timeout_ms = 0;
    }
    s->used = true;
    if (s->nxfer < USBHOST_ISO_MIN_XFER) {
        usbhost_stream_stop_locked(s);
        return -MP_ENOMEM;
    }
    if (!in) {
        usbhost_pcm_sink_open(&usbhost_stream_gates[usbhost_stream_index(s)], cfg->frame);
    }
    s->open = true;
    for (int i = 0; i < s->nxfer; i++) {
        if (in) {
            usbhost_stream_arm_in(s, s->xfer[i]);
        } else {
            usbhost_stream_fill(s, s->xfer[i]);
        }
        if (usb_host_transfer_submit(s->xfer[i]) == ESP_OK) {
            s->inflight++;
        } else {
            s->st.errors++;
        }
    }
    if (s->inflight == 0) {
        usbhost_stream_stop_locked(s);
        return -MP_EIO;
    }
    printf("usbhost_stream: ep 0x%02x itf %u %s, %u-byte packets, %u transfers of %u%s\n",
        (unsigned)ep, (unsigned)itf, in ? "in" : "out", (unsigned)packet, (unsigned)s->nxfer,
        (unsigned)s->pkts, cfg->video ? ", video frames" : s->paced ? ", paced" : cfg->downmix ? ", to mono" : "");
    return usbhost_stream_index(s);
}

int usbhost_stream_open(uint32_t dev_id, uint8_t ep, const usbhost_stream_cfg_t *cfg) {
    usbhost_host_lock();
    int r = usbhost_stream_open_locked(dev_id, ep, cfg);
    usbhost_host_unlock();
    return r;
}

int usbhost_stream_close(uint32_t dev_id, uint8_t ep) {
    usbhost_host_lock();
    usbhost_stream_t *s = usbhost_stream_find(dev_id, ep);
    int r = -MP_ENOENT;
    if (s != NULL) {
        r = usbhost_stream_index(s);
        usbhost_stream_stop_locked(s);
        if (s->used) {
            r = -MP_EBUSY;      // transfers that never came back
        }
    }
    usbhost_host_unlock();
    return r;
}

int usbhost_stream_stats(uint32_t dev_id, uint8_t ep, usbhost_stream_stats_t *out) {
    usbhost_stream_t *s = usbhost_stream_find(dev_id, ep);
    if (s == NULL) {
        return -MP_ENOENT;
    }
    memcpy(out, (const void *)&s->st, sizeof(*out));
    if (s->cfg.video) {
        out->frames = s->vid.frames;
        out->dropped = s->vid.dropped_full;
        out->torn = s->vid.dropped_torn;
        out->big = s->vid.dropped_big;
    }
    return usbhost_stream_index(s);
}

static usbhost_stream_t *usbhost_stream_out(uint32_t dev_id, uint8_t ep, int *err) {
    usbhost_stream_t *s = usbhost_stream_find(dev_id, ep);
    if (s == NULL || !s->open) {
        *err = -MP_ENOENT;
        return NULL;
    }
    if (s->in) {
        *err = -MP_EINVAL;
        return NULL;
    }
    return s;
}

int usbhost_stream_write(uint32_t dev_id, uint8_t ep, const uint8_t *data, size_t len) {
    int err;
    usbhost_stream_t *s = usbhost_stream_out(dev_id, ep, &err);
    if (s == NULL) {
        return err;
    }
    usbhost_pcm_sink_t *g = &usbhost_stream_gates[usbhost_stream_index(s)];
    int n = usbhost_pcm_sink_write(g, usbhost_pcm_sink_current(g), data, len);
    return n < 0 ? -MP_ENOENT : n;
}

int usbhost_stream_space(uint32_t dev_id, uint8_t ep) {
    int err;
    usbhost_stream_t *s = usbhost_stream_out(dev_id, ep, &err);
    if (s == NULL) {
        return err;
    }
    usbhost_pcm_sink_t *g = &usbhost_stream_gates[usbhost_stream_index(s)];
    int n = usbhost_pcm_sink_space(g, usbhost_pcm_sink_current(g));
    return n < 0 ? -MP_ENOENT : n;
}

// --- the C sink -------------------------------------------------------------
//
// What another native module gets from Speaker.c_sink(): pcm_c_sink.h's
// struct, its ctx the slot and the stream's generation (never a pointer into
// the heap). Safe from any task; once the stream stops, -1 for good.

#define USBHOST_STREAM_CTX(slot, gen) ((void *)(uintptr_t)(((uintptr_t)(gen) << 2) | (uintptr_t)(slot)))

static int usbhost_stream_c_write(void *ctx, const uint8_t *data, size_t len) {
    const uintptr_t c = (uintptr_t)ctx;
    return usbhost_pcm_sink_write(&usbhost_stream_gates[c & 3], (uint32_t)(c >> 2), data, len);
}

static int usbhost_stream_c_space(void *ctx) {
    const uintptr_t c = (uintptr_t)ctx;
    return usbhost_pcm_sink_space(&usbhost_stream_gates[c & 3], (uint32_t)(c >> 2));
}

int usbhost_stream_c_sink(uint32_t dev_id, uint8_t ep, pcm_c_sink_t *out, uint32_t channels, uint32_t bits) {
    int err;
    usbhost_stream_t *s = usbhost_stream_out(dev_id, ep, &err);
    if (s == NULL) {
        return err;
    }
    const int slot = usbhost_stream_index(s);
    const uint32_t gen = usbhost_pcm_sink_current(&usbhost_stream_gates[slot]);
    if (gen == 0 || gen > (UINT32_MAX >> 2) || s->cfg.frame == 0) {
        return -MP_EINVAL;
    }
    memset(out, 0, sizeof(*out));
    out->magic = PCM_C_SINK_MAGIC;
    out->version = PCM_C_SINK_VERSION;
    out->ctx = USBHOST_STREAM_CTX(slot, gen);
    out->write = usbhost_stream_c_write;
    out->space = usbhost_stream_c_space;
    out->rate = s->cfg.rate;
    out->channels = channels;
    out->bits = bits;
    out->frame_bytes = s->cfg.frame;
    return 0;
}

#endif // CONFIG_SOC_USB_OTG_SUPPORTED
