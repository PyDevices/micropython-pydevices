// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// CircuitPython's two recording classes for a board without I2S, over the
// STM32H7 capture in audioif_stm32.c:
//
//     import array, audiobusio, board
//     buf = array.array("H", [0] * 16000)
//     with audiobusio.PDMIn(board.MICROPHONE_CLOCK, board.MICROPHONE_DATA,
//                           sample_rate=16000, bit_depth=16) as mic:
//         mic.record(buf, len(buf))
//
//     import analogbufio
//     with analogbufio.BufferedIn("PA0", sample_rate=16000) as adc:
//         adc.readinto(buf)
//
// `audiobusio.PDMIn` reads a PDM microphone through DFSDM, which does the
// decimation in hardware; `analogbufio.BufferedIn` reads an analog pin on
// ADC1 at a steady rate. Their arguments and buffers are CircuitPython's, so
// code written for it runs with only its pins changed. The two share one
// capture ring, so only one records at a time.
//
// PDMIn's samples have the DC taken out, as CircuitPython's do, and its 0 dB
// puts the microphone's full scale at the buffer's; an unsigned buffer rests
// at its midpoint. BufferedIn's are the ADC's own 16-bit values, untouched.

#include <string.h>

#include "py/binary.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/runtime.h"

#include "cp_compat/objproperty.h"

#include "audiopump_i2s.h"

#if defined(AUDIOIF_DRIVER_STM32)

// How long a recording waits for anything at all before it gives back what
// it has: the DMA is clocked by us, so this is a stopped peripheral.
#define AUDIOIN_STALL_MS (1000)

// Fill `n` samples of 16-bit from the ring into `dst`, blocking; fewer if
// the ring stalls. Signed unless `flip`, which moves them to unsigned.
static uint32_t audioin_fill(void *dst, uint32_t n, uint8_t bytes, bool flip) {
    int16_t scratch[256];
    uint32_t done = 0;
    mp_uint_t last = mp_hal_ticks_ms();
    while (done < n) {
        uint32_t want = n - done;
        if (want > MP_ARRAY_SIZE(scratch)) {
            want = MP_ARRAY_SIZE(scratch);
        }
        uint32_t got = audiopump_i2s_rx_pull((uint8_t *)scratch, want * 2) / 2;
        if (got == 0) {
            if (mp_hal_ticks_ms() - last > AUDIOIN_STALL_MS) {
                break;
            }
            // Runs the scheduler, so the audio pump goes on playing, and
            // raises a pending KeyboardInterrupt.
            mp_event_wait_ms(2);
            continue;
        }
        last = mp_hal_ticks_ms();
        for (uint32_t i = 0; i < got; i++) {
            uint16_t u = (uint16_t)scratch[i] ^ (flip ? 0x8000u : 0u);
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

// --- audiobusio.PDMIn ------------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    bool deinited;
    uint8_t bit_depth;
    uint32_t sample_rate;
} audioin_pdmin_obj_t;

static void audioin_pdmin_check(audioin_pdmin_obj_t *self) {
    if (self->deinited) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Object has been deinitialized and can no longer be used. "
            "Create a new object."));
    }
}

static mp_obj_t audioin_pdmin_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_clock_pin, ARG_data_pin, ARG_sample_rate, ARG_bit_depth, ARG_mono,
           ARG_oversample, ARG_startup_delay };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_clock_pin,     MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_data_pin,      MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_sample_rate,   MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 16000 } },
        { MP_QSTR_bit_depth,     MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 8 } },
        { MP_QSTR_mono,          MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
        // CircuitPython's decimation; here the clock and the decimation are
        // chosen together to land on sample_rate, so it is only checked.
        { MP_QSTR_oversample,    MP_ARG_KW_ONLY | MP_ARG_INT,  { .u_int = 64 } },
        { MP_QSTR_startup_delay, MP_ARG_KW_ONLY | MP_ARG_OBJ,  { .u_obj = MP_OBJ_NULL } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    const mp_int_t depth = args[ARG_bit_depth].u_int;
    if (depth != 8 && depth != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("bit_depth must be 8 or 16"));
    }
    if (!args[ARG_mono].u_bool) {
        mp_raise_ValueError(MP_ERROR_TEXT("only mono: the board has one PDM microphone"));
    }
    if (args[ARG_oversample].u_int <= 0 || args[ARG_oversample].u_int % 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("oversample must be a multiple of 8"));
    }
    mp_float_t delay = 0.11f;
    if (args[ARG_startup_delay].u_obj != MP_OBJ_NULL) {
        delay = mp_obj_get_float(args[ARG_startup_delay].u_obj);
    }
    if (delay < 0 || delay > 1) {
        mp_raise_ValueError(MP_ERROR_TEXT("startup_delay must be 0 to 1 second"));
    }

    audioin_pdmin_obj_t *self = mp_obj_malloc_with_finaliser(audioin_pdmin_obj_t, type);
    self->deinited = true;   // until the microphone is running
    self->bit_depth = (uint8_t)depth;
    audioif_rx_set_gain(0.0f);
    self->sample_rate = audioif_rx_pdm_begin(pin_find(args[ARG_clock_pin].u_obj),
        pin_find(args[ARG_data_pin].u_obj), (uint32_t)args[ARG_sample_rate].u_int, AUDIOIF_PDM_EDGE);
    self->deinited = false;
    // CircuitPython waits here for the microphone to wake; what it said
    // while it did is thrown away.
    mp_hal_delay_ms((mp_uint_t)(delay * 1000));
    audiopump_i2s_rx_discard();
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioin_pdmin_deinit(mp_obj_t self_in) {
    audioin_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited) {
        audiopump_i2s_rx_end();
        self->deinited = true;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioin_pdmin_deinit_obj, audioin_pdmin_deinit);

static mp_obj_t audioin_pdmin_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audioin_pdmin_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audioin_pdmin_exit_obj, 4, 4, audioin_pdmin_exit);

// CircuitPython's buffers: 'H' at 16 bits, 'B' or a bytearray at 8. A
// signed 'h' or 'b' is taken too, and gets signed samples.
static mp_obj_t audioin_pdmin_record(mp_obj_t self_in, mp_obj_t dest_in, mp_obj_t length_in) {
    audioin_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioin_pdmin_check(self);
    mp_int_t length = mp_obj_get_int(length_in);
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
    bool flip = true;
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
    uint32_t n = audioin_fill(buf.buf, (uint32_t)length, self->bit_depth / 8, flip);
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audioin_pdmin_record_obj, audioin_pdmin_record);

// The rate the microphone really runs at: the nearest DFSDM can divide to.
static mp_obj_t audioin_pdmin_get_sample_rate(mp_obj_t self_in) {
    audioin_pdmin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioin_pdmin_check(self);
    return mp_obj_new_int_from_uint(self->sample_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioin_pdmin_get_sample_rate_obj, audioin_pdmin_get_sample_rate);
MP_PROPERTY_GETTER(audioin_pdmin_sample_rate_obj, (mp_obj_t)&audioin_pdmin_get_sample_rate_obj);

static const mp_rom_map_elem_t audioin_pdmin_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audioin_pdmin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioin_pdmin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audioin_pdmin_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_record), MP_ROM_PTR(&audioin_pdmin_record_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&audioin_pdmin_sample_rate_obj) },
};
static MP_DEFINE_CONST_DICT(audioin_pdmin_locals, audioin_pdmin_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiobusio_pdmin_type,
    MP_QSTR_PDMIn,
    MP_TYPE_FLAG_NONE,
    make_new, audioin_pdmin_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioin_pdmin_locals
    );

// --- analogbufio.BufferedIn ------------------------------------------------

typedef struct {
    mp_obj_base_t base;
    bool deinited;
    const machine_pin_obj_t *pin;
    uint32_t sample_rate;
} audioin_bufferedin_obj_t;

static void audioin_bufferedin_check(audioin_bufferedin_obj_t *self) {
    if (self->deinited) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Object has been deinitialized and can no longer be used. "
            "Create a new object."));
    }
}

static mp_obj_t audioin_bufferedin_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_pin, ARG_sample_rate };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_pin,         MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_sample_rate, MP_ARG_REQUIRED | MP_ARG_KW_ONLY | MP_ARG_INT, { .u_int = 0 } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);
    const machine_pin_obj_t *pin = pin_find(args[ARG_pin].u_obj);
    if (!(pin->adc_num & PIN_ADC1)) {
        mp_raise_ValueError(MP_ERROR_TEXT("that pin has no ADC1 channel"));
    }
    if (args[ARG_sample_rate].u_int < 1000 || args[ARG_sample_rate].u_int > 96000) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate out of range"));
    }
    audioin_bufferedin_obj_t *self = mp_obj_malloc_with_finaliser(audioin_bufferedin_obj_t, type);
    self->deinited = false;
    self->pin = pin;
    self->sample_rate = (uint32_t)args[ARG_sample_rate].u_int;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audioin_bufferedin_deinit(mp_obj_t self_in) {
    audioin_bufferedin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->deinited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioin_bufferedin_deinit_obj, audioin_bufferedin_deinit);

static mp_obj_t audioin_bufferedin_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audioin_bufferedin_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audioin_bufferedin_exit_obj, 4, 4,
    audioin_bufferedin_exit);

// CircuitPython's: fill the whole buffer, 'H' with 16-bit values or 'B' (or
// a bytearray) with their top bytes, and return how many samples it holds.
static mp_obj_t audioin_bufferedin_readinto(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_buffer, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_buffer, MP_ARG_REQUIRED | MP_ARG_OBJ, { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_loop,   MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
    };
    audioin_bufferedin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    audioin_bufferedin_check(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);
    if (args[ARG_loop].u_bool) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("loop=True"));
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_buffer].u_obj, &buf, MP_BUFFER_WRITE);
    uint8_t bytes;
    if (buf.typecode == 'H') {
        bytes = 2;
    } else if (buf.typecode == 'B' || buf.typecode == BYTEARRAY_TYPECODE) {
        bytes = 1;
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer must be an array of type 'H' or 'B', or a bytearray"));
    }
    uint32_t n = buf.len / bytes;
    self->sample_rate = audioif_rx_adc_begin(self->pin, self->sample_rate, true);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        n = audioin_fill(buf.buf, n, bytes, false);
        nlr_pop();
    } else {
        audiopump_i2s_rx_end();
        nlr_jump(nlr.ret_val);
    }
    audiopump_i2s_rx_end();
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audioin_bufferedin_readinto_obj, 2, audioin_bufferedin_readinto);

// The rate the timer really runs at, after the first readinto().
static mp_obj_t audioin_bufferedin_get_sample_rate(mp_obj_t self_in) {
    audioin_bufferedin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioin_bufferedin_check(self);
    return mp_obj_new_int_from_uint(self->sample_rate);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioin_bufferedin_get_sample_rate_obj,
    audioin_bufferedin_get_sample_rate);
MP_PROPERTY_GETTER(audioin_bufferedin_sample_rate_obj,
    (mp_obj_t)&audioin_bufferedin_get_sample_rate_obj);

static const mp_rom_map_elem_t audioin_bufferedin_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audioin_bufferedin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioin_bufferedin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audioin_bufferedin_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&audioin_bufferedin_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate), MP_ROM_PTR(&audioin_bufferedin_sample_rate_obj) },
};
static MP_DEFINE_CONST_DICT(audioin_bufferedin_locals, audioin_bufferedin_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    audioin_bufferedin_type,
    MP_QSTR_BufferedIn,
    MP_TYPE_FLAG_NONE,
    make_new, audioin_bufferedin_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioin_bufferedin_locals
    );

static const mp_rom_map_elem_t analogbufio_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_analogbufio) },
    { MP_ROM_QSTR(MP_QSTR_BufferedIn), MP_ROM_PTR(&audioin_bufferedin_type) },
};
static MP_DEFINE_CONST_DICT(analogbufio_globals, analogbufio_globals_table);

const mp_obj_module_t analogbufio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&analogbufio_globals,
};
MP_REGISTER_MODULE(MP_QSTR_analogbufio, analogbufio_module);

#endif // AUDIOIF_DRIVER_STM32
