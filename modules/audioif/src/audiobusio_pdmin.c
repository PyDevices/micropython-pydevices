// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiobusio.PDMIn on an ESP32: CircuitPython's PDM microphone class, over the
// I2S peripheral's PDM receive mode.
//
//     import array, audiobusio
//     buf = array.array("H", [0] * 16000)
//     with audiobusio.PDMIn(44, 47, sample_rate=16000, bit_depth=16) as mic:
//         mic.record(buf, len(buf))
//
// The constructor, `record()`, `sample_rate` and the errors are CircuitPython's
// (shared-bindings/audiobusio/PDMIn.c, and its espressif common-hal, which
// opens the same I2S0 channel). The decimation is the peripheral's own
// PDM-to-PCM filter, so `oversample` is checked and not used, as it is there.
// Only I2S0 has that filter, on the ESP32, S3 and P4; elsewhere (the C3 and
// C6, which have none) making one raises NotImplementedError.
//
// UNDER IT is the capture half the I2S microphone class already uses: the DMA
// interrupt copies each finished descriptor into a ring, and `record()` empties
// the ring into the caller's buffer, so a script that stops for a garbage
// collection between two calls loses nothing. See audiopump_i2s.h.
//
// Two small differences from CircuitPython's espressif port, both toward its
// own documentation: `record()` returns the number of samples, where the
// espressif code returns bytes, and a 16-bit 'H' buffer is offset-binary (rests
// at 32768) as on SAMD, RP2040 and this firmware's STM32 PDMIn. A signed 'h'
// or 'b' buffer is taken too, and gets signed samples. Neither port removes the
// filter's DC offset; that is left to the caller.

#include <string.h>

#include "py/binary.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/runtime.h"

#include "cp_compat/objproperty.h"

#include "soc/soc_caps.h"

#include "audiopump_i2s.h"

#if SOC_I2S_SUPPORTS_PDM_RX

// How long a recording waits for anything at all before it gives back what
// it has: the DMA is clocked by us, so this is a stopped peripheral.
#define AUDIOPDM_STALL_MS (1000)

// The ring, in time: a quarter of a second, between 4 kB and 64 kB.
#define AUDIOPDM_RING_MS (250)

typedef struct {
    mp_obj_base_t base;
    bool deinited;
    bool mono;
    uint8_t bit_depth;
    uint32_t sample_rate;
} audiopdm_pdmin_obj_t;

static void audiopdm_check(audiopdm_pdmin_obj_t *self) {
    if (self->deinited) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Object has been deinitialized and can no longer be used. "
            "Create a new object."));
    }
}

// Fill `n` samples from the ring into `dst`, blocking; fewer if the ring
// stalls. The filter's samples are signed 16-bit; `flip` moves them to
// offset-binary, and a 1-byte sample keeps the top byte.
static uint32_t audiopdm_fill(void *dst, uint32_t n, uint8_t bytes, bool flip,
    bool stereo) {
    int16_t scratch[256];
    uint32_t done = 0;
    mp_uint_t last = mp_hal_ticks_ms();
    while (done < n) {
        uint32_t want = n - done;
        if (want > MP_ARRAY_SIZE(scratch)) {
            want = MP_ARRAY_SIZE(scratch);
        }
        if (stereo) {
            want &= ~1u;   // whole frames only
        }
        uint32_t got = audiopump_i2s_rx_pull((uint8_t *)scratch, want * 2) / 2;
        if (got == 0) {
            if (mp_hal_ticks_ms() - last > AUDIOPDM_STALL_MS) {
                break;
            }
            // Runs the scheduler, so the audio pump goes on playing, and
            // raises a pending KeyboardInterrupt.
            mp_event_wait_ms(2);
            continue;
        }
        last = mp_hal_ticks_ms();
        for (uint32_t i = 0; i < got; i++) {
            const uint16_t u = (uint16_t)scratch[i] ^ (flip ? 0x8000u : 0u);
            if (bytes == 1) {
                ((uint8_t *)dst)[done + i] = (uint8_t)(u >> 8);
            } else {
                ((uint16_t *)dst)[done + i] = u;
            }
        }
        done += got;
    }
    return done;
}

static mp_obj_t audiopdm_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_clock_pin, ARG_data_pin, ARG_sample_rate, ARG_bit_depth,
           ARG_mono, ARG_oversample, ARG_startup_delay };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_clock_pin,     MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_data_pin,      MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_sample_rate,   MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 16000 } },
        { MP_QSTR_bit_depth,     MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 8 } },
        { MP_QSTR_mono,          MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        { MP_QSTR_oversample,    MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 64 } },
        { MP_QSTR_startup_delay, MP_ARG_KW_ONLY | MP_ARG_OBJ,  { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    const int clock = audiobusio_pin_gpio(args[ARG_clock_pin].u_obj,
        MP_QSTR_clock_pin);
    const int data = audiobusio_pin_gpio(args[ARG_data_pin].u_obj,
        MP_QSTR_data_pin);
    if (clock < 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("clock_pin must be a pin"));
    }
    if (data < 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("data_pin must be a pin"));
    }
    if (args[ARG_sample_rate].u_int < 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate must be >= 1"));
    }
    const mp_int_t depth = args[ARG_bit_depth].u_int;
    if (depth != 8 && depth != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bit_depth must be multiple of 8."));
    }
    const mp_int_t over = args[ARG_oversample].u_int;
    if (over < 8 || over > 255 || over % 8 != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("oversample must be multiple of 8."));
    }
    mp_float_t delay = 0.11f;
    if (args[ARG_startup_delay].u_obj != MP_OBJ_NULL) {
        delay = mp_obj_get_float(args[ARG_startup_delay].u_obj);
    }
    if (delay < 0 || delay > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("startup_delay must be 0 to 1 second"));
    }
    if (audiopump_i2s_rx_held()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peripheral in use"));
    }

    const bool mono = args[ARG_mono].u_bool;
    const uint32_t rate = (uint32_t)args[ARG_sample_rate].u_int;
    const audiopump_i2s_cfg_t cfg = {
        .port = 0,
        .bclk = clock,
        .ws = -1,
        .dout = -1,
        .rate = (int)rate,
        .bits = 16,
        .channels = 2,
        .mclk = -1,
        .dma_desc = 6,
        .dma_frame = audiopump_i2s_dma_frame((int)rate),
        .din = data,
        .in_port = -1,
        .in_bclk = -1,
        .in_ws = -1,
        .in_mclk = -1,
        .pdm = 1,
    };
    const uint32_t slots = mono ? 1 : 2;
    uint32_t ring = rate * 2 * slots * AUDIOPDM_RING_MS / 1000;
    ring = ring < 4096 ? 4096 : (ring > 65536 ? 65536 : ring);

    audiopdm_pdmin_obj_t *self = mp_obj_malloc_with_finaliser(
        audiopdm_pdmin_obj_t, type);
    self->deinited = true;   // until the microphone is running
    self->mono = mono;
    self->bit_depth = (uint8_t)depth;
    self->sample_rate = rate;
    (void)audiopump_i2s_rx_begin(&cfg, ring, mono ? 0 : 3);
    self->deinited = false;
    // The microphone wakes up in this time, and what it said while it did
    // is thrown away.
    mp_hal_delay_ms((mp_uint_t)(delay * 1000));
    audiopump_i2s_rx_discard();
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiopdm_deinit(mp_obj_t self_in) {
    audiopdm_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited) {
        audiopump_i2s_rx_end();
        self->deinited = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopdm_deinit_obj, audiopdm_deinit);

static mp_obj_t audiopdm_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audiopdm_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiopdm_exit_obj, 4, 4,
    audiopdm_exit);

static mp_obj_t audiopdm_record(mp_obj_t self_in, mp_obj_t dest_in,
    mp_obj_t length_in) {
    audiopdm_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiopdm_check(self);
    const mp_int_t length = mp_obj_get_int(length_in);
    if (length < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("length must be >= 0"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(dest_in, &buf, MP_BUFFER_WRITE);
    const size_t item = mp_binary_get_size('@', buf.typecode, NULL);
    if (buf.len / item < (size_t)length) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Destination capacity is smaller than destination_length."));
    }
    const char tc = (char)buf.typecode;
    bool flip;
    if (self->bit_depth == 16) {
        if (tc != 'H' && tc != 'h') {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "destination buffer must be an array of type 'H' for bit_depth = 16"));
        }
        flip = tc == 'H';
    } else {
        if (tc != 'B' && tc != 'b' && buf.typecode != BYTEARRAY_TYPECODE) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "destination buffer must be a bytearray or array of type 'B' for bit_depth = 8"));
        }
        flip = tc != 'b';
    }
    uint32_t n = (uint32_t)length;
    if (!self->mono) {
        n &= ~1u;   // two slots a frame
    }
    n = audiopdm_fill(buf.buf, n, self->bit_depth / 8, flip, !self->mono);
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audiopdm_record_obj, audiopdm_record);

static mp_obj_t audiopdm_get_sample_rate(mp_obj_t self_in) {
    audiopdm_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiopdm_check(self);
    return mp_obj_new_int_from_uint(self->sample_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiopdm_get_sample_rate_obj,
    audiopdm_get_sample_rate);
MP_PROPERTY_GETTER(audiopdm_sample_rate_obj,
    (mp_obj_t)&audiopdm_get_sample_rate_obj);

static const mp_rom_map_elem_t audiopdm_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiopdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiopdm_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiopdm_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_record), MP_ROM_PTR(&audiopdm_record_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&audiopdm_sample_rate_obj) },
};
static MP_DEFINE_CONST_DICT(audiopdm_locals, audiopdm_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiobusio_pdmin_type,
    MP_QSTR_PDMIn,
    MP_TYPE_FLAG_NONE,
    make_new, audiopdm_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiopdm_locals
    );

#else  // no PDM receive on this chip

static mp_obj_t audiopdm_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *args) {
    (void)type;
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("PDMIn"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    audiobusio_pdmin_type,
    MP_QSTR_PDMIn,
    MP_TYPE_FLAG_NONE,
    make_new, audiopdm_make_new
    );

#endif
