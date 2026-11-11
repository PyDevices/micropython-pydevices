// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT
//
// Runs one esp_cam_sensor driver on a PC and prints every register access it
// makes, so sensorgen.py can turn Espressif's own code into a sensor
// description without anyone reading the C and copying numbers out of it.
//
// sensorgen.py compiles this file together with one driver's .c, the host
// stand-ins in this directory and the firmware's own sdkconfig.h, then reads
// what it prints:
//
//   M <phase> [arg]           the start of a phase
//   W <reg> <value> <bits> <abits>   a register write (hex): value and address widths
//   R <reg> <value> <bits> <abits>   a register read and what the chip answered
//   D <ms>                    a delay
//   T <timer name> <ms>       the driver started a background timer
//   F <json>                  a format the driver offers
//   P <json>                  a control the driver offers, for the current format
//   E <phase> <esp_err>       a call that failed (or "runaway": it never stopped)
//
// The chip is an array of registers: a write stores, a read returns what was
// stored. Two probes override that, so a read-modify-write shows its mask:
// with READ_ZERO every read answers 0, with READ_ONES every read answers all
// ones.

#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cam_sensor.h"

#ifndef DRIVER_DETECT
#error "define DRIVER_DETECT, the driver's detect function"
#endif
#ifndef DRIVER_PID
#error "define DRIVER_PID, the product ID the driver checks"
#endif

esp_cam_sensor_device_t *DRIVER_DETECT(esp_cam_sensor_config_t *config);

static uint32_t mem[65536];
enum { READ_MEM, READ_ZERO, READ_ONES };
static int read_mode = READ_MEM;
static long ops;                 // accesses in this phase
static jmp_buf runaway;
static int quiet;                // detect's first, ID-finding pass prints nothing
static uint16_t id_reads[8];
static int id_bits[8];
static int n_id_reads;

#define OPS_MAX 200000L

static void count(void) {
    if (++ops > OPS_MAX) {
        longjmp(runaway, 1);
    }
}

static void out(const char *fmt, ...) {
    if (quiet) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void trace_delay_ms(uint32_t ms) {
    count();
    out("D %u\n", (unsigned)ms);
}

static esp_err_t wr(uint16_t r, uint32_t v, int bits, int abits) {
    count();
    mem[r] = v;
    out("W %04x %x %d %d\n", r, (unsigned)v, bits, abits);
    return ESP_OK;
}

static uint32_t rd(uint16_t r, int bits, int abits) {
    count();
    uint32_t mask = bits >= 32 ? 0xffffffffu : ((1u << bits) - 1);
    uint32_t v = read_mode == READ_ZERO ? 0 : read_mode == READ_ONES ? mask : (mem[r] & mask);
    if (quiet && n_id_reads < 8) {
        id_bits[n_id_reads] = bits;
        id_reads[n_id_reads++] = r;
    }
    out("R %04x %x %d %d\n", r, (unsigned)v, bits, abits);
    return v;
}

esp_err_t esp_sccb_transmit_reg_a8v8(esp_sccb_io_handle_t h, uint8_t r, uint8_t v) { return wr(r, v, 8, 8); }
esp_err_t esp_sccb_transmit_reg_a16v8(esp_sccb_io_handle_t h, uint16_t r, uint8_t v) { return wr(r, v, 8, 16); }
esp_err_t esp_sccb_transmit_reg_a8v16(esp_sccb_io_handle_t h, uint8_t r, uint16_t v) { return wr(r, v, 16, 8); }
esp_err_t esp_sccb_transmit_reg_a16v16(esp_sccb_io_handle_t h, uint16_t r, uint16_t v) { return wr(r, v, 16, 16); }
esp_err_t esp_sccb_transmit_reg_a16v32(esp_sccb_io_handle_t h, uint16_t r, uint32_t v) { return wr(r, v, 32, 16); }
esp_err_t esp_sccb_transmit_reg_a16(esp_sccb_io_handle_t h, uint16_t r, const void *v, size_t n) {
    const uint8_t *b = v;
    for (size_t i = 0; i < n; i++) {
        wr(r + i, b[i], 8, 16);
    }
    return ESP_OK;
}
esp_err_t esp_sccb_transmit_receive_reg_a8v8(esp_sccb_io_handle_t h, uint8_t r, uint8_t *v) { *v = rd(r, 8, 8); return ESP_OK; }
esp_err_t esp_sccb_transmit_receive_reg_a16v8(esp_sccb_io_handle_t h, uint16_t r, uint8_t *v) { *v = rd(r, 8, 16); return ESP_OK; }
esp_err_t esp_sccb_transmit_receive_reg_a8v16(esp_sccb_io_handle_t h, uint8_t r, uint16_t *v) { *v = rd(r, 16, 8); return ESP_OK; }
esp_err_t esp_sccb_transmit_receive_reg_a16v16(esp_sccb_io_handle_t h, uint16_t r, uint16_t *v) { *v = rd(r, 16, 16); return ESP_OK; }
esp_err_t esp_sccb_transmit_receive_reg_a16v32(esp_sccb_io_handle_t h, uint16_t r, uint32_t *v) { *v = rd(r, 32, 16); return ESP_OK; }
esp_err_t esp_sccb_transmit_receive_reg_a16(esp_sccb_io_handle_t h, uint16_t r, void *v, size_t n) {
    uint8_t *b = v;
    for (size_t i = 0; i < n; i++) {
        b[i] = rd(r + i, 8, 16);
    }
    return ESP_OK;
}
esp_err_t esp_sccb_transmit_v16(esp_sccb_io_handle_t h, uint16_t v) { return wr(0xffff, v, 16, 16); }
esp_err_t esp_sccb_receive_v16(esp_sccb_io_handle_t h, uint16_t *v) { *v = rd(0xffff, 16, 16); return ESP_OK; }
esp_err_t esp_sccb_del_i2c_io(esp_sccb_io_handle_t h) { return ESP_OK; }

esp_err_t gpio_config(const gpio_config_t *c) { return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { return 0; }

typedef struct { const char *name; uint32_t ms; void *id; } host_timer_t;
TimerHandle_t xTimerCreate(const char *n, TickType_t p, UBaseType_t r, void *id, TimerCallbackFunction_t cb) {
    host_timer_t *t = calloc(1, sizeof(*t));
    t->name = n;
    t->ms = p;
    t->id = id;
    return t;
}
void *pvTimerGetTimerID(TimerHandle_t t) { return ((host_timer_t *)t)->id; }
BaseType_t xTimerStart(TimerHandle_t t, TickType_t w) {
    out("T %s %u\n", ((host_timer_t *)t)->name, (unsigned)((host_timer_t *)t)->ms);
    return pdTRUE;
}
BaseType_t xTimerStop(TimerHandle_t t, TickType_t w) { return pdTRUE; }
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t w) { free(t); return pdTRUE; }

// --- the phases -------------------------------------------------------------

static esp_cam_sensor_device_t *dev;
static esp_cam_sensor_config_t cfg;

static esp_cam_sensor_device_t *detect(void) {
    cfg.sccb_handle = (esp_sccb_io_handle_t)1;
    cfg.reset_pin = -1;
    cfg.pwdn_pin = -1;
    cfg.xclk_pin = -1;
    cfg.sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return DRIVER_DETECT(&cfg);
}

// Registers whose reset value a driver reads and acts on: TRACE_PRELOAD is
// "reg=value,reg=value" in hex, the chip's value for them before any write.
static void preload(void) {
    const char *p = getenv("TRACE_PRELOAD");
    while (p && *p) {
        char *end;
        unsigned long r = strtoul(p, &end, 16);
        if (*end != '=') {
            break;
        }
        unsigned long v = strtoul(end + 1, &end, 16);
        mem[r & 0xffff] = v;
        p = *end == ',' ? end + 1 : end;
    }
}

// Answer the ID check: run detect once silently to see which registers it
// reads, put the product ID there, and run it for real.
static void seed_id(void) {
    memset(mem, 0, sizeof(mem));
    quiet = 1;
    n_id_reads = 0;
    ops = 0;
    if (setjmp(runaway) == 0) {
        esp_cam_sensor_device_t *d = detect();
        if (d && d->ops && d->ops->del) {
            d->ops->del(d);
        }
    }
    quiet = 0;
    memset(mem, 0, sizeof(mem));
    preload();
    uint32_t pid = DRIVER_PID;
    if (n_id_reads >= 2 && id_bits[0] == 8) {
        // high byte first, as every 8-bit-register driver here reads it
        mem[id_reads[0]] = (pid >> 8) & 0xff;
        mem[id_reads[1]] = pid & 0xff;
    } else if (n_id_reads >= 1) {
        mem[id_reads[0]] = pid;
    }
}

static const struct { uint32_t id; const char *name; } params[] = {
    { ESP_CAM_SENSOR_EXPOSURE_VAL, "exposure_val" },
    { ESP_CAM_SENSOR_EXPOSURE_US, "exposure_us" },
    { ESP_CAM_SENSOR_GAIN, "gain" },
    { ESP_CAM_SENSOR_ANGAIN, "angain" },
    { ESP_CAM_SENSOR_DGAIN, "dgain" },
    { ESP_CAM_SENSOR_GROUP_EXP_GAIN, "group_exp_gain" },
    { ESP_CAM_SENSOR_VFLIP, "vflip" },
    { ESP_CAM_SENSOR_HMIRROR, "hmirror" },
    { ESP_CAM_SENSOR_AE_LEVEL, "ae_level" },
    { ESP_CAM_SENSOR_AE_CONTROL, "ae_control" },
    { ESP_CAM_SENSOR_AGC, "agc" },
    { ESP_CAM_SENSOR_AWB, "awb" },
    { ESP_CAM_SENSOR_FPS, "fps" },
    { ESP_CAM_SENSOR_STATS, "stats" },
    { ESP_CAM_SENSOR_BRIGHTNESS, "brightness" },
    { ESP_CAM_SENSOR_CONTRAST, "contrast" },
    { ESP_CAM_SENSOR_SATURATION, "saturation" },
    { ESP_CAM_SENSOR_HUE, "hue" },
    { ESP_CAM_SENSOR_SHARPNESS, "sharpness" },
    { ESP_CAM_SENSOR_DENOISE, "denoise" },
    { ESP_CAM_SENSOR_SPECIAL_EFFECT, "special_effect" },
    { ESP_CAM_SENSOR_AE_FLICKER, "ae_flicker" },
};
#define N_PARAMS (sizeof(params) / sizeof(params[0]))

static esp_cam_sensor_param_desc_t descs[N_PARAMS];
static bool have_desc[N_PARAMS];

static void query_params(void) {
    for (size_t i = 0; i < N_PARAMS; i++) {
        esp_cam_sensor_param_desc_t *q = &descs[i];
        memset(q, 0, sizeof(*q));
        q->id = params[i].id;
        have_desc[i] = dev->ops->query_para_desc && dev->ops->query_para_desc(dev, q) == ESP_OK;
        if (!have_desc[i]) {
            continue;
        }
        printf("P {\"name\": \"%s\", \"type\": %d", params[i].name, (int)q->type);
        if (q->type == ESP_CAM_SENSOR_PARAM_TYPE_NUMBER) {
            printf(", \"min\": %ld, \"max\": %ld, \"step\": %ld", (long)q->number.minimum,
                (long)q->number.maximum, (long)q->number.step);
        } else if (q->type == ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION) {
            printf(", \"count\": %u, \"elements\": [", (unsigned)q->enumeration.count);
            for (uint32_t k = 0; k < q->enumeration.count; k++) {
                printf("%s%lu", k ? ", " : "", (unsigned long)q->enumeration.elements[k]);
            }
            printf("]");
        } else if (q->type == ESP_CAM_SENSOR_PARAM_TYPE_U8) {
            printf(", \"size\": %u", (unsigned)q->u8.size);
        }
        printf(", \"default\": %ld}\n", (long)q->default_value);
    }
}

static void set_param(const char *name, uint32_t id, int32_t v) {
    out("M %s %ld\n", name, (long)v);
    ops = 0;
    if (setjmp(runaway) != 0) {
        out("E %s runaway\n", name);
        return;
    }
    esp_err_t e = dev->ops->set_para_value(dev, id, &v, sizeof(v));
    if (e != ESP_OK) {
        out("E %s %d\n", name, e);
    }
}

static void ioctl_int(const char *name, uint32_t cmd, int v) {
    out("M %s %d\n", name, v);
    ops = 0;
    if (setjmp(runaway) != 0) {
        out("E %s runaway\n", name);
        return;
    }
    esp_err_t e = dev->ops->priv_ioctl(dev, cmd, &v);
    if (e != ESP_OK) {
        out("E %s %d\n", name, e);
    }
}

// A control's register writes, with every read answering 0 and then all ones.
static void probe_bits(const char *name, uint32_t id, int ioctl_cmd) {
    static const char *modes[] = { "zero", "ones" };
    uint32_t *saved = malloc(sizeof(mem));
    memcpy(saved, mem, sizeof(mem));
    for (int m = 0; m < 2; m++) {
        read_mode = m == 0 ? READ_ZERO : READ_ONES;
        for (int v = 1; v >= 0; v--) {   // on first: a driver that remembers the stream skips a repeat
            char phase[48];
            snprintf(phase, sizeof(phase), "%s_%s", name, modes[m]);
            if (ioctl_cmd) {
                ioctl_int(phase, ioctl_cmd, v);
            } else {
                set_param(phase, id, v);
            }
        }
    }
    read_mode = READ_MEM;
    memcpy(mem, saved, sizeof(mem));
    free(saved);
}

static int cmp_long(const void *a, const void *b) {
    long x = *(const long *)a, y = *(const long *)b;
    return x < y ? -1 : x > y;
}

// Values to try on a numeric control: every one when there are few, else
// the ends, each power of two and its neighbours, and a spread between.
static size_t probe_values(long lo, long hi, long *out_v, size_t cap) {
    size_t n = 0;
    if (hi - lo <= 4096) {
        for (long v = lo; v <= hi && n < cap; v++) {
            out_v[n++] = v;
        }
        return n;
    }
    out_v[n++] = lo;
    out_v[n++] = hi;
    for (int b = 0; b < 31 && n + 3 < cap; b++) {
        long p = 1L << b;
        for (long d = -1; d <= 1; d++) {
            if (p + d >= lo && p + d <= hi) {
                out_v[n++] = p + d;
            }
        }
    }
    for (int k = 1; k < 64 && n < cap; k++) {
        out_v[n++] = lo + (hi - lo) * k / 64;
    }
    qsort(out_v, n, sizeof(long), cmp_long);
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m == 0 || out_v[i] != out_v[m - 1]) {
            out_v[m++] = out_v[i];
        }
    }
    return m;
}

static void probe_values_of(size_t i) {
    static long vals[8192];
    esp_cam_sensor_param_desc_t *q = &descs[i];
    size_t n = 0;
    if (q->type == ESP_CAM_SENSOR_PARAM_TYPE_NUMBER) {
        n = probe_values(q->number.minimum, q->number.maximum, vals, 8192);
    } else if (q->type == ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION) {
        for (uint32_t k = 0; k < q->enumeration.count && k < 8192; k++) {
            vals[n++] = k;     // an enumeration is set by its index
        }
    }
    for (size_t k = 0; k < n; k++) {
        set_param(params[i].name, params[i].id, (int32_t)vals[k]);
    }
    // A few of the values again with every read answering 0, then all ones,
    // so a read-modify-write on a register shows its mask.
    if (n > 0) {
        size_t pick[6] = { 0, n > 1 ? 1 : 0, n / 3, n / 2, n > 1 ? n - 2 : 0, n - 1 };
        static const char *suffix[] = { "rz", "ro" };
        uint32_t *saved = malloc(sizeof(mem));
        memcpy(saved, mem, sizeof(mem));
        for (int m = 0; m < 2; m++) {
            read_mode = m == 0 ? READ_ZERO : READ_ONES;
            char phase[48];
            snprintf(phase, sizeof(phase), "%s_%s", params[i].name, suffix[m]);
            // A control with few settings may be a table, so every one; else a spread.
            size_t count = n <= 700 ? n : 6;
            for (size_t k = 0; k < count; k++) {
                size_t at = n <= 700 ? k : pick[k];
                if (n > 700 && k > 0 && pick[k] == pick[k - 1]) {
                    continue;
                }
                set_param(phase, params[i].id, (int32_t)vals[at]);
            }
        }
        read_mode = READ_MEM;
        memcpy(mem, saved, sizeof(mem));
        free(saved);
    }
}

// A driver that dereferences something the host doesn't provide ends the
// phase it's in, not the whole trace.
static void crashed(int sig) {
    fputs("E crash\n", stdout);
    longjmp(runaway, 2);
}

int main(int argc, char **argv) {
    int only_format = argc > 1 ? atoi(argv[1]) : -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa = { .sa_handler = crashed, .sa_flags = SA_NODEFER };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);      // a division by a register that read 0
    seed_id();
    uint32_t id_mem[65536 / 32];      // which registers hold the ID
    (void)id_mem;
    uint32_t *seeded = malloc(sizeof(mem));
    memcpy(seeded, mem, sizeof(mem));

    out("M detect\n");
    ops = 0;
    if (setjmp(runaway) != 0) {
        out("E detect runaway\n");
        return 2;
    }
    dev = detect();
    if (dev == NULL) {
        out("E detect null\n");
        return 2;
    }
    printf("M detected %s %x\n", dev->name ? dev->name : "?", (unsigned)dev->id.pid);

    esp_cam_sensor_format_array_t fa = {0};
    dev->ops->query_support_formats(dev, &fa);
    for (uint32_t i = 0; i < fa.count; i++) {
        const esp_cam_sensor_format_t *f = &fa.format_array[i];
        const esp_cam_sensor_isp_info_t *isp = f->isp_info;
        printf("F {\"index\": %u, \"name\": \"%s\", \"format\": %d, \"port\": %d, \"xclk\": %lu, "
            "\"width\": %u, \"height\": %u, \"fps\": %u, \"mipi_clk\": %lu, \"lanes\": %u, "
            "\"line_sync\": %d, \"regs_size\": %u",
            (unsigned)i, f->name, (int)f->format, (int)f->port, (unsigned long)f->xclk,
            (unsigned)f->width, (unsigned)f->height, (unsigned)f->fps,
            (unsigned long)f->mipi_info.mipi_clk, (unsigned)f->mipi_info.lane_num,
            (int)f->mipi_info.line_sync_en, (unsigned)f->regs_size);
        if (isp) {
            printf(", \"pclk\": %lu, \"hts\": %lu, \"vts\": %lu, \"tline_ns\": %lu, "
                "\"gain_def\": %lu, \"exp_def\": %lu, \"bayer\": %d",
                (unsigned long)isp->isp_v1_info.pclk, (unsigned long)isp->isp_v1_info.hts,
                (unsigned long)isp->isp_v1_info.vts, (unsigned long)isp->isp_v1_info.tline_ns,
                (unsigned long)isp->isp_v1_info.gain_def, (unsigned long)isp->isp_v1_info.exp_def,
                (int)isp->isp_v1_info.bayer_type);
        }
        printf("}\n");
    }

    for (uint32_t i = 0; i < fa.count; i++) {
        const esp_cam_sensor_format_t *f = &fa.format_array[i];
        if (f->port != ESP_CAM_SENSOR_MIPI_CSI || (only_format >= 0 && (int)i != only_format)) {
            continue;
        }
        memcpy(mem, seeded, sizeof(mem));
        out("M format %u\n", (unsigned)i);
        ops = 0;
        if (setjmp(runaway) != 0) {
            out("E format runaway\n");
            continue;
        }
        esp_err_t e = dev->ops->set_format(dev, f);
        if (e != ESP_OK) {
            out("E format %d\n", e);
            continue;
        }
        query_params();
        // Stream, test pattern and flips against the registers the mode left.
        ioctl_int("stream", ESP_CAM_SENSOR_IOC_S_STREAM, 1);
        ioctl_int("stream", ESP_CAM_SENSOR_IOC_S_STREAM, 0);
        probe_bits("stream", 0, ESP_CAM_SENSOR_IOC_S_STREAM);
        probe_bits("test_pattern", 0, ESP_CAM_SENSOR_IOC_S_TEST_PATTERN);
        for (size_t k = 0; k < N_PARAMS; k++) {
            if (!have_desc[k]) {
                continue;
            }
            if (params[k].id == ESP_CAM_SENSOR_VFLIP || params[k].id == ESP_CAM_SENSOR_HMIRROR) {
                probe_bits(params[k].name, params[k].id, 0);
            } else if (params[k].id != ESP_CAM_SENSOR_STATS && params[k].id != ESP_CAM_SENSOR_GROUP_EXP_GAIN) {
                probe_values_of(k);
                // Each probe starts from the mode as set, not the last probe.
            }
        }
        // Set the mode again with every read answering 0, then all ones: if
        // what it writes changes, the mode depends on what the chip says.
        for (volatile int m = 0; m < 2; m++) {
            const char *how = m == 0 ? "zero" : "ones";
            memcpy(mem, seeded, sizeof(mem));
            read_mode = m == 0 ? READ_ZERO : READ_ONES;
            out("M format_%s %u\n", how, (unsigned)i);
            ops = 0;
            if (setjmp(runaway) != 0) {
                out("E format_%s runaway\n", how);
            } else if ((e = dev->ops->set_format(dev, f)) != ESP_OK) {
                out("E format_%s %d\n", how, e);
            }
            read_mode = READ_MEM;
        }
        memcpy(mem, seeded, sizeof(mem));
        ioctl_int("sw_reset", ESP_CAM_SENSOR_IOC_SW_RESET, 0);
    }
    return 0;
}
