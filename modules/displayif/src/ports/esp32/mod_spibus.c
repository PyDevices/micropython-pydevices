// SPDX-License-Identifier: MIT
// spibus.SPIBus for ESP32 targets: esp_lcd panel IO over SPI, colour data by DMA.
//
// Same class, keywords and send() as the common machine.SPI bus
// (src/ports/common/spi/mod_spibus.c), so board configs run unchanged. What
// differs is when send() returns: data larger than a parameter is copied into
// the bus's own DMA buffers and queued, and send() returns while the panel is
// still being written. The caller's buffer is free again as soon as send()
// returns. A later command, or more data than the buffers hold, waits for the
// transfers ahead of it. busy() and wait() say when the wire is idle.
//
// The bus falls back to the common machine.SPI implementation, with the same
// methods, where esp_lcd can't or shouldn't own the SPI host: soft=True, no
// command (D/C) pin (9-bit mode), bits other than 8, no sck/mosi given, a miso
// pin given (the bus is likely shared with touch or an SD card through
// machine.SPI), dma=False, or no internal RAM for the DMA buffers.
//
// Lifecycle: idempotent deinit/__del__/ctor + soft-reset teardown (soft_reset.h).
// Everything the transfer-done interrupt touches is file-static, never in the
// GC heap, so a bus object that is collected can't be written by an interrupt.

#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpprint.h"
#include "displayif/build.h"
#include "displayif/mp_helpers.h"
#include "displayif/soft_reset.h"
#include "displayif_esp32_pins.h"

#include "sdkconfig.h"
#include "soc/soc_caps.h"
#include "esp_attr.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DC_CMD 0
#define DC_DATA 1
#define CS_ACTIVE 0
#define CS_INACTIVE 1

// Data up to this many bytes is a parameter: sent at once, by polling, from a
// small internal buffer. Anything longer is queued for DMA.
#define SPIBUS_PARAM_MAX 64

// DMA buffering: one internal block, sent in transfers of SPIBUS_CHUNK bytes.
// Data that fits the block goes as one queued send with chip select held
// throughout, as the blocking bus does. Larger data streams through the block
// a chunk at a time. The P4 has internal RAM to spare and a 240x240 RGB565
// frame (115,200 bytes) fits in 128 KB, so a full-frame send returns once
// it's copied; elsewhere 32 KB, as qspibus uses. Halved in turn when internal
// RAM is short, down to two chunks.
#define SPIBUS_CHUNK (16U * 1024U)
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define SPIBUS_RING_MAX (128U * 1024U)
#else
#define SPIBUS_RING_MAX (32U * 1024U)
#endif
#define SPIBUS_WAIT_TIMEOUT_MS 1000U

typedef struct _spibus_dma_t {
    bool active;
    spi_host_device_t host;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t done_sem;
    uint8_t *param_buf;
    uint8_t *ring;
    size_t ring_size;
    uint32_t queued;          // colour transfers queued (task side)
    volatile uint32_t done;   // colour transfers finished (interrupt side)
} spibus_dma_t;

typedef struct _spibus_obj_t {
    mp_obj_base_t base;
    mp_obj_t fallback;  // common machine.SPI SPIBus, or MP_OBJ_NULL on the DMA path
    mp_obj_t reset;     // machine.Pin or None
    mp_obj_t callback;  // register_callback(), or None
    bool has_reset;
    bool deinited;
} spibus_obj_t;

static const mp_obj_type_t spibus_type;

// The common bus, compiled on ESP32 without its module registration.
extern const mp_obj_type_t *const displayif_spibus_machine_type_ptr;
void displayif_spibus_machine_teardown(void);

static spibus_dma_t s_dma;
static spibus_obj_t *s_owner; // compared, never dereferenced (may be collected)
static bool s_soft_reset_registered;

// ---------------------------------------------------------------------------
// DMA state (file-static)

static bool IRAM_ATTR spibus_on_color_done(esp_lcd_panel_io_handle_t io,
    esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    (void)io;
    (void)edata;
    (void)user_ctx;
    s_dma.done++;
    BaseType_t woken = pdFALSE;
    if (s_dma.done_sem != NULL) {
        xSemaphoreGiveFromISR(s_dma.done_sem, &woken);
    }
    // esp_lcd's SPI path ignores what this callback returns, so a task that
    // waits in spibus_wait_inflight would otherwise run again only at the
    // next tick. Ask for the switch here.
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return woken == pdTRUE;
}

static inline uint32_t spibus_inflight(void) {
    // Signed: after a timeout resets the count, a late completion can't wrap it.
    int32_t n = (int32_t)(s_dma.queued - s_dma.done);
    return n > 0 ? (uint32_t)n : 0;
}

// Wait until at most `limit` colour transfers are in flight. False on timeout.
// The GIL is released while waiting, so other threads keep running.
static bool spibus_wait_inflight(uint32_t limit) {
    bool ok = true;
    if (spibus_inflight() <= limit) {
        return true;
    }
    MP_THREAD_GIL_EXIT();
    while (spibus_inflight() > limit) {
        if (xSemaphoreTake(s_dma.done_sem, pdMS_TO_TICKS(SPIBUS_WAIT_TIMEOUT_MS)) != pdTRUE
            && spibus_inflight() > limit) {
            ok = false;
            break;
        }
    }
    MP_THREAD_GIL_ENTER();
    return ok;
}

static void spibus_dma_release(void) {
    if (s_dma.io != NULL) {
        // Let queued transfers finish (bounded) before the device goes away;
        // panel_io_del also recycles them.
        if (s_dma.done_sem != NULL) {
            uint32_t tries = 0;
            while (spibus_inflight() > 0 && tries++ < 4) {
                xSemaphoreTake(s_dma.done_sem, pdMS_TO_TICKS(SPIBUS_WAIT_TIMEOUT_MS / 4));
            }
        }
        esp_lcd_panel_io_del(s_dma.io);
        s_dma.io = NULL;
    }
    if (s_dma.active) {
        spi_bus_free(s_dma.host);
    }
    heap_caps_free(s_dma.ring);
    s_dma.ring = NULL;
    heap_caps_free(s_dma.param_buf);
    s_dma.param_buf = NULL;
    if (s_dma.done_sem != NULL) {
        vSemaphoreDelete(s_dma.done_sem);
        s_dma.done_sem = NULL;
    }
    s_dma.active = false;
    s_dma.ring_size = 0;
    s_dma.queued = 0;
    s_dma.done = 0;
}

// The biggest block to ask for: SPIBUS_RING_MAX, or what `dma=<bytes>` said.
static size_t s_ring_max = SPIBUS_RING_MAX;

static bool spibus_dma_alloc_buffers(void) {
    s_dma.param_buf = heap_caps_aligned_alloc(64, SPIBUS_PARAM_MAX, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_dma.param_buf == NULL) {
        return false;
    }
    size_t start = s_ring_max > SPIBUS_RING_MAX ? SPIBUS_RING_MAX : s_ring_max;
    if (start < 2 * SPIBUS_CHUNK) {
        start = 2 * SPIBUS_CHUNK;
    }
    for (size_t size = start; size >= 2 * SPIBUS_CHUNK; size /= 2) {
        s_dma.ring = heap_caps_aligned_alloc(64, size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_dma.ring != NULL) {
            s_dma.ring_size = size;
            return true;
        }
    }
    return false;
}

// Bring up the host for the DMA path. Returns ESP_OK, ESP_ERR_NO_MEM when the
// buffers don't fit (the caller falls back), or the esp_lcd / SPI error.
static esp_err_t spibus_dma_open(spi_host_device_t host, int sck, int mosi, int cs, int dc,
    int mode, uint32_t hz, bool lsb_first) {
    memset(&s_dma, 0, sizeof(s_dma));
    s_dma.host = host;
    s_dma.done_sem = xSemaphoreCreateBinary();
    if (s_dma.done_sem == NULL || !spibus_dma_alloc_buffers()) {
        spibus_dma_release();
        return ESP_ERR_NO_MEM;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = sck,
        .mosi_io_num = mosi,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPIBUS_CHUNK,
    };
    esp_err_t err = spi_bus_initialize(host, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        spibus_dma_release();
        return err;
    }
    s_dma.active = true;

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = cs,
        .dc_gpio_num = dc,
        .spi_mode = mode,
        .pclk_hz = hz,
        // One slot per chunk of the block, so a send that fits it is queued
        // whole without esp_lcd waiting for a slot.
        .trans_queue_depth = s_dma.ring_size / SPIBUS_CHUNK + 1,
        .on_color_trans_done = spibus_on_color_done,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            .lsb_first = lsb_first,
        },
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)host, &io_config, &s_dma.io);
    if (err != ESP_OK) {
        spibus_dma_release();
        return err;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle

static void spibus_deinit_internal(spibus_obj_t *self) {
    if (self == NULL || self->deinited) {
        return;
    }
    if (self->fallback != MP_OBJ_NULL) {
        displayif_obj_call_method0(self->fallback, MP_QSTR_deinit);
    } else if (s_owner == self) {
        spibus_dma_release();
    }
    self->deinited = true;
    if (s_owner == self) {
        s_owner = NULL;
    }
}

static void spibus_host_teardown(void) {
    // Soft reset or a new SPIBus: release the DMA host without touching the
    // (possibly collected) object that owned it.
    spibus_dma_release();
    s_owner = NULL;
}

static void spibus_ensure_soft_reset_registered(void) {
    if (!s_soft_reset_registered) {
        displayif_register_soft_reset(spibus_host_teardown);
        s_soft_reset_registered = true;
    }
}

static void spibus_check(spibus_obj_t *self) {
    if (self->deinited || (self->fallback == MP_OBJ_NULL && (s_owner != self || !s_dma.active))) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("spibus is deinited"));
    }
}

static void spibus_raise_esp_err(esp_err_t err) {
    mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("ESP-IDF error %d (%s)"), err, esp_err_to_name(err));
}

static void spibus_raise_timeout(void) {
    // The transfers never finished: forget them so the bus can be used again.
    s_dma.done = s_dma.queued;
    mp_raise_OSError(MP_ETIMEDOUT);
}

// ---------------------------------------------------------------------------
// Constructor

// Keyword-only, as the common bus, plus dma=None|True|False:
//   SPIBus(*, id=2, baudrate=24_000_000, polarity=0, phase=0, bits=8,
//           lsb_first=False, soft=False, sck=-1, mosi=-1, miso=-1,
//           command=-1, chip_select=-1, reset=-1, dma=None)
static mp_obj_t spibus_make(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    if (n_args != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("extra positional arguments given"));
    }

    mp_int_t id = 2;
    mp_int_t baudrate = 24000000;
    mp_int_t polarity = 0;
    mp_int_t phase = 0;
    mp_int_t bits = 8;
    bool lsb_first = false;
    bool soft = false;
    mp_obj_t sck = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t mosi = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t miso = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t command = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t chip_select = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t reset = MP_OBJ_NEW_SMALL_INT(-1);
    mp_obj_t dma = mp_const_none;
    size_t dma_kw = n_kw;

    for (size_t i = 0; i < n_kw; i++) {
        mp_obj_t key = args[2 * i];
        mp_obj_t val = args[2 * i + 1];
        if (!mp_obj_is_qstr(key)) {
            mp_raise_TypeError(MP_ERROR_TEXT("keywords must be strings"));
        }
        qstr q = MP_OBJ_QSTR_VALUE(key);
        if (q == MP_QSTR_id) {
            id = mp_obj_get_int(val);
        } else if (q == MP_QSTR_baudrate) {
            baudrate = mp_obj_get_int(val);
        } else if (q == MP_QSTR_polarity) {
            polarity = mp_obj_get_int(val);
        } else if (q == MP_QSTR_phase) {
            phase = mp_obj_get_int(val);
        } else if (q == MP_QSTR_bits) {
            bits = mp_obj_get_int(val);
        } else if (q == MP_QSTR_lsb_first) {
            lsb_first = mp_obj_is_true(val);
        } else if (q == MP_QSTR_soft) {
            soft = mp_obj_is_true(val);
        } else if (q == MP_QSTR_sck) {
            sck = val;
        } else if (q == MP_QSTR_mosi) {
            mosi = val;
        } else if (q == MP_QSTR_miso) {
            miso = val;
        } else if (q == MP_QSTR_command) {
            command = val;
        } else if (q == MP_QSTR_chip_select) {
            chip_select = val;
        } else if (q == MP_QSTR_reset) {
            reset = val;
        } else if (q == MP_QSTR_dma) {
            dma = val;
            dma_kw = i;
        } else {
            mp_raise_TypeError(MP_ERROR_TEXT("extra keyword arguments given"));
        }
    }

    bool dma_able = !soft && bits == 8
        && !displayif_pin_spec_unset(command)
        && !displayif_pin_spec_unset(sck) && !displayif_pin_spec_unset(mosi)
        && id >= 1 && id < SOC_SPI_PERIPH_NUM;
    bool want_dma;
    s_ring_max = SPIBUS_RING_MAX;
    if (dma == mp_const_none) {
        want_dma = dma_able && displayif_pin_spec_unset(miso);
    } else if (mp_obj_is_int(dma) && mp_obj_get_int(dma) > 1) {
        // dma=<bytes>: DMA on, with at most this much internal RAM queued
        // (rounded down to a power of two of 16 KB chunks, at least 32 KB).
        // A board that needs its internal RAM elsewhere asks for less.
        want_dma = true;
        s_ring_max = (size_t)mp_obj_get_int(dma);
        if (!dma_able) {
            mp_raise_ValueError(MP_ERROR_TEXT("dma needs id, sck, mosi and command, bits=8 and soft=False"));
        }
    } else {
        want_dma = mp_obj_is_true(dma);
        if (want_dma && !dma_able) {
            mp_raise_ValueError(MP_ERROR_TEXT("dma needs id, sck, mosi and command, bits=8 and soft=False"));
        }
    }

    // Tear down any prior SPIBus of either kind (soft-reset survivor or leaked).
    spibus_ensure_soft_reset_registered();
    spibus_host_teardown();
    displayif_spibus_machine_teardown();

    spibus_obj_t *self = mp_obj_malloc(spibus_obj_t, type);
    self->fallback = MP_OBJ_NULL;
    self->reset = mp_const_none;
    self->callback = mp_const_none;
    self->has_reset = false;
    self->deinited = false;

    if (want_dma) {
        mp_printf(&mp_plat_print, "SPIBus loading...\n");
        int cs = displayif_pin_spec_unset(chip_select) ? -1 : displayif_esp32_pin_gpio(chip_select);
        esp_err_t err = spibus_dma_open((spi_host_device_t)id,
            displayif_esp32_pin_gpio(sck), displayif_esp32_pin_gpio(mosi), cs,
            displayif_esp32_pin_gpio(command), (int)((polarity << 1) | phase),
            (uint32_t)baudrate, lsb_first);
        if ((err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE) && dma == mp_const_none) {
            // No internal RAM for the buffers, or machine.SPI already holds
            // the host: the blocking bus, which shares it.
            want_dma = false;
        } else if (err == ESP_ERR_INVALID_STATE) {
            mp_raise_ValueError(MP_ERROR_TEXT("SPI bus in use"));
        } else if (err != ESP_OK) {
            spibus_raise_esp_err(err);
        }
    }

    if (!want_dma) {
        // The common bus takes the same keywords, without dma.
        mp_obj_t kw[2 * n_kw + 1];
        size_t m = 0;
        for (size_t i = 0; i < n_kw; i++) {
            if (i != dma_kw) {
                kw[2 * m] = args[2 * i];
                kw[2 * m + 1] = args[2 * i + 1];
                m++;
            }
        }
        self->fallback = mp_call_function_n_kw(MP_OBJ_FROM_PTR(displayif_spibus_machine_type_ptr), 0, m, kw);
        return MP_OBJ_FROM_PTR(self);
    }

    s_owner = self;

    if (!displayif_pin_spec_unset(reset)) {
        mp_obj_t pin_cls = mp_load_attr(mp_import_name(MP_QSTR_machine, DISPLAYIF_EMPTY_DICT, MP_OBJ_NULL), MP_QSTR_Pin);
        mp_int_t pin_out = mp_obj_get_int(mp_load_attr(pin_cls, MP_QSTR_OUT));
        self->reset = displayif_machine_pin_cfg(reset, pin_out, 1);
        self->has_reset = true;
        // Match CircuitPython FourWire: pulse reset on construct.
        displayif_pin_set(self->reset, 0);
        mp_hal_delay_us(1000);
        displayif_pin_set(self->reset, 1);
        mp_hal_delay_us(1000);
    }

    mp_printf(&mp_plat_print, "SPIBus loaded (C, DMA, %u bytes queued)\n", (unsigned)s_dma.ring_size);
    return MP_OBJ_FROM_PTR(self);
}

// ---------------------------------------------------------------------------
// Methods

static mp_obj_t spibus_reset(mp_obj_t self_in) {
    spibus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fallback != MP_OBJ_NULL && !self->deinited) {
        return displayif_obj_call_method0(self->fallback, MP_QSTR_reset);
    }
    spibus_check(self);
    if (!self->has_reset) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("No %q pin"), MP_QSTR_reset);
    }
    displayif_pin_set(self->reset, 0);
    mp_hal_delay_us(1000);
    displayif_pin_set(self->reset, 1);
    mp_hal_delay_us(1000);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(spibus_reset_obj, spibus_reset);

static void spibus_tx_color(int cmd, const uint8_t *buf, size_t len) {
    s_dma.queued++;
    esp_err_t err = esp_lcd_panel_io_tx_color(s_dma.io, cmd, buf, len);
    if (err != ESP_OK) {
        s_dma.queued--;
        spibus_raise_esp_err(err);
    }
}

// Copy `len` bytes into the DMA block and queue them, after `cmd` unless it
// is -1. Waits first for the transfers ahead, which also keeps the order of
// commands and data on the wire.
static void spibus_queue_data(int cmd, const uint8_t *data, size_t len) {
    if (!spibus_wait_inflight(0)) {
        spibus_raise_timeout();
    }
    if (len <= s_dma.ring_size) {
        // One send: esp_lcd splits it into chunks and holds chip select.
        memcpy(s_dma.ring, data, len);
        spibus_tx_color(cmd, s_dma.ring, len);
        return;
    }
    // Larger than the block: stream it a chunk at a time, reusing a chunk
    // once the transfer that held it is done.
    const uint32_t slots = s_dma.ring_size / SPIBUS_CHUNK;
    uint32_t slot = 0;
    while (len > 0) {
        if (!spibus_wait_inflight(slots - 1)) {
            spibus_raise_timeout();
        }
        size_t n = len > SPIBUS_CHUNK ? SPIBUS_CHUNK : len;
        uint8_t *buf = s_dma.ring + slot * SPIBUS_CHUNK;
        memcpy(buf, data, n);
        spibus_tx_color(cmd, buf, n);
        slot = (slot + 1) % slots;
        cmd = -1;
        data += n;
        len -= n;
    }
}

static mp_obj_t spibus_send(size_t n_args, const mp_obj_t *args) {
    spibus_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_obj_t command = (n_args > 1) ? args[1] : mp_const_none;
    mp_obj_t data = (n_args > 2) ? args[2] : mp_const_none;

    if (self->fallback != MP_OBJ_NULL && !self->deinited) {
        mp_obj_t dest[4];
        mp_load_method(self->fallback, MP_QSTR_send, dest);
        dest[2] = command;
        dest[3] = data;
        mp_call_method_n_kw(2, 0, dest);
    } else {
        spibus_check(self);
        int cmd = (command != mp_const_none) ? (int)(uint8_t)mp_obj_get_int(command) : -1;

        // Match Python: `if data and len(data):`
        mp_buffer_info_t bufinfo = { .buf = NULL, .len = 0 };
        if (data != mp_const_none && data != mp_const_false && data != mp_const_empty_bytes) {
            mp_get_buffer(data, &bufinfo, MP_BUFFER_READ);
        }

        if (bufinfo.len > SPIBUS_PARAM_MAX) {
            spibus_queue_data(cmd, bufinfo.buf, bufinfo.len);
        } else if (cmd >= 0 || bufinfo.len > 0) {
            if (!spibus_wait_inflight(0)) {
                spibus_raise_timeout();
            }
            memcpy(s_dma.param_buf, bufinfo.buf, bufinfo.len);
            esp_err_t err = esp_lcd_panel_io_tx_param(s_dma.io, cmd,
                bufinfo.len ? s_dma.param_buf : NULL, bufinfo.len);
            if (err != ESP_OK) {
                spibus_raise_esp_err(err);
            }
        }
    }

    // The caller's buffer is free now: tell whoever registered for that.
    if (self->callback != mp_const_none && data != mp_const_none) {
        mp_call_function_0(self->callback);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(spibus_send_obj, 1, 3, spibus_send);

static mp_obj_t spibus_busy(mp_obj_t self_in) {
    spibus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fallback != MP_OBJ_NULL || self->deinited || s_owner != self) {
        return mp_const_false;
    }
    return mp_obj_new_bool(spibus_inflight() > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(spibus_busy_obj, spibus_busy);

static mp_obj_t spibus_wait(mp_obj_t self_in) {
    spibus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fallback != MP_OBJ_NULL || self->deinited || s_owner != self) {
        return mp_const_none;
    }
    if (!spibus_wait_inflight(0)) {
        spibus_raise_timeout();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(spibus_wait_obj, spibus_wait);

static mp_obj_t spibus_register_callback(mp_obj_t self_in, mp_obj_t callback) {
    spibus_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (callback != mp_const_none && !mp_obj_is_callable(callback)) {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable or None"));
    }
    self->callback = callback;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(spibus_register_callback_obj, spibus_register_callback);

static mp_obj_t spibus_deinit(mp_obj_t self_in) {
    spibus_deinit_internal(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(spibus_deinit_obj, spibus_deinit);

static const mp_rom_map_elem_t spibus_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&spibus_reset_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&spibus_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_busy), MP_ROM_PTR(&spibus_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait), MP_ROM_PTR(&spibus_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_register_callback), MP_ROM_PTR(&spibus_register_callback_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&spibus_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&spibus_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(spibus_locals_dict, spibus_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    spibus_type,
    MP_QSTR_SPIBus,
    MP_TYPE_FLAG_NONE,
    make_new, spibus_make,
    locals_dict, &spibus_locals_dict
);

static const mp_rom_map_elem_t spibus_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_spibus) },
    DISPLAYIF_REVISION_ENTRY,
    { MP_ROM_QSTR(MP_QSTR_SPIBus), MP_ROM_PTR(&spibus_type) },
    { MP_ROM_QSTR(MP_QSTR_DC_CMD), MP_ROM_INT(DC_CMD) },
    { MP_ROM_QSTR(MP_QSTR_DC_DATA), MP_ROM_INT(DC_DATA) },
    { MP_ROM_QSTR(MP_QSTR_CS_ACTIVE), MP_ROM_INT(CS_ACTIVE) },
    { MP_ROM_QSTR(MP_QSTR_CS_INACTIVE), MP_ROM_INT(CS_INACTIVE) },
};
static MP_DEFINE_CONST_DICT(spibus_module_globals, spibus_module_globals_table);

const mp_obj_module_t spibus_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&spibus_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_spibus, spibus_user_cmodule);
