// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audioi2sin -- CircuitPython's I2S microphone class, over audioif's capture
// half.
//
//     import array, audioi2sin, board
//     buf = array.array("h", [0] * 16000)
//     with audioi2sin.I2SIn(board.D9, board.D10, board.D11,
//                           sample_rate=16000, bit_depth=16) as mic:
//         mic.record(buf, len(buf))
//
// That is the example in CircuitPython's own `shared-bindings/audioi2sin/
// I2SIn.c` docstring, and it runs here with only its pins changed.
// CircuitPython keeps I2S capture in this module of its own, not in
// `audiobusio`, so this is where code written for it looks.
//
// WHAT IS UNDER IT. The RX half `_audioif` already had for recording while
// an output plays: the DMA's interrupt copies every finished descriptor into
// a ring, and `record()` empties the ring into the caller's buffer, converting
// as it goes. Three ways the channel can come to be (audiopump_i2s.h has them
// in full): a receive-only channel of its own, which is what CircuitPython's
// constructor opens and what this one opens when nothing else is; the RX half
// of an `audiobusio.I2SOut(..., data_in=)` on the same pins, which it joins, so
// a board whose codec and microphone share one port records while it plays;
// or a receive-only channel on its own pins beside an output somewhere else.
//
// The ring is what makes `record()` safe to call from Python at all. The DMA
// holds 30 ms; a script that stops for a garbage collection between two calls
// would lose the rest. The ring holds a quarter of a second.
//
// THE CODEC IS NOT HERE. CircuitPython's I2SIn does not know a codec exists,
// and neither does this: a MEMS microphone needs nothing, and a board whose
// microphone is an ADC behind I2C (the ES7210 on the T-Embed and the ESP32-P4
// panels) has it brought up by its board configuration first, the way the
// board's `audio_power` role powers its DAC for `audiobusio.I2SOut`.
//
// ON A DESKTOP there is no I2S. The class is the same, and `record()` reads a
// buffer of wire frames handed in as `source=` (two slots a frame, at
// `bit_depth`), or silence paced off the clock without one. That is what
// makes the conversions and the lifecycle testable with no board.

#include <stdint.h>
#include <string.h>

#include "py/binary.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/runtime.h"

#include "cp_compat/objproperty.h"

#include "audiopump_i2s.h"

// How long `record()` waits for the microphone to deliver anything at all
// before it gives back what it has. Our channel is the clock master, so this
// is a dead DMA rather than a slow source; CircuitPython's espressif port
// would block forever.
#define AUDIOI2SIN_STALL_MS (1000)

// The ring, in time: a quarter of a second, between 4 kB and 64 kB.
#define AUDIOI2SIN_RING_MS (250)

typedef struct _audioi2sin_i2sin_obj_t {
    mp_obj_base_t base;
    bool deinited;
    bool recording;         // holds the capture ring (board builds)
    bool mono;
    bool samples_signed;
    bool overflow;          // since `overflow` was last read
    uint8_t bit_depth;      // on the wire: 8, 16, 24 or 32
    uint8_t out_depth;      // in the caller's buffer
    uint8_t slot_bytes;     // one sample in the ring: 1, 2 or 4 bytes
    uint32_t sample_rate;
    uint32_t dropped_seen;  // the ring's dropped count at the last look
    // --- desktop -------------------------------------------------------
    mp_obj_t source;        // a buffer of wire frames, or MP_OBJ_NULL
    size_t source_at;       // where the next frame is read, in bytes
} audioi2sin_i2sin_obj_t;

extern const mp_obj_type_t audioi2sin_i2sin_type;

static void audioi2sin_check(audioi2sin_i2sin_obj_t *self) {
    if (self->deinited) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Object has been deinitialized and can no longer be used. "
            "Create a new object."));
    }
}

static bool audioi2sin_depth_ok(mp_int_t v) {
    return v == 8 || v == 16 || v == 24 || v == 32;
}

// --- the conversion, CircuitPython's own -----------------------------------
//
// A sample off the ring, sign-extended to an int32 at `in_depth` scale. A
// 24-bit microphone (INMP441, SPH0645LM4H) sends its sample in the top 24
// bits of a 32-bit slot, so the bus is opened at 32 bits for it and the low
// byte, which carries nothing, is shifted away.
static inline int32_t audioi2sin_read(const uint8_t *p, uint8_t slot_bytes,
    uint8_t in_depth) {
    if (slot_bytes == 1) {
        return *(const int8_t *)p;
    }
    if (slot_bytes == 2) {
        int16_t v;
        memcpy(&v, p, sizeof(v));
        return v;
    }
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return in_depth == 24 ? (v >> 8) : v;
}

// CircuitPython's `shared_audioi2sin_write_converted`
// (shared-module/audioi2sin/__init__.h), arithmetic for arithmetic, so the
// same microphone fills the same buffer with the same numbers on either
// firmware: widening bit-replicates, so full scale stays full scale;
// narrowing shifts right; unsigned flips the sign bit at the output width.
static inline void audioi2sin_store(void *buffer, uint32_t idx, int32_t s,
    uint8_t in_depth, uint8_t out_depth, bool samples_signed) {
    int32_t shifted;
    if (out_depth > in_depth) {
        uint32_t top = (uint32_t)s << (32 - in_depth);
        top |= top >> in_depth;
        if (in_depth * 2 < out_depth) {
            top |= top >> (in_depth * 2);
        }
        shifted = (int32_t)top >> (32 - out_depth);
    } else if (out_depth == in_depth) {
        shifted = s;
    } else {
        shifted = s >> (in_depth - out_depth);
    }
    uint32_t u = (uint32_t)shifted;
    if (!samples_signed) {
        if (out_depth >= 32) {
            u ^= 0x80000000u;
        } else {
            const uint32_t mask = (1u << out_depth) - 1u;
            u = (u & mask) ^ (1u << (out_depth - 1));
        }
    }
    switch (out_depth) {
        case 8:
            ((uint8_t *)buffer)[idx] = (uint8_t)(u & 0xffu);
            break;
        case 16:
            ((uint16_t *)buffer)[idx] = (uint16_t)(u & 0xffffu);
            break;
        default:
            ((uint32_t *)buffer)[idx] = u;
            break;
    }
}

// --- the constructor -------------------------------------------------------

static mp_obj_t audioi2sin_i2sin_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bit_clock, ARG_word_select, ARG_data, ARG_main_clock,
           ARG_sample_rate, ARG_bit_depth, ARG_output_bit_depth, ARG_mono,
           ARG_left_justified, ARG_samples_signed, ARG_external_clock,
           ARG_port, ARG_main_clock_fs, ARG_source };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bit_clock,        MP_ARG_REQUIRED | MP_ARG_OBJ,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_word_select,      MP_ARG_REQUIRED | MP_ARG_OBJ,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_data,             MP_ARG_REQUIRED | MP_ARG_OBJ,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_main_clock,       MP_ARG_KW_ONLY | MP_ARG_OBJ,
          { .u_obj = mp_const_none } },
        { MP_QSTR_sample_rate,      MP_ARG_KW_ONLY | MP_ARG_INT,
          { .u_int = 16000 } },
        { MP_QSTR_bit_depth,        MP_ARG_KW_ONLY | MP_ARG_INT,
          { .u_int = 16 } },
        { MP_QSTR_output_bit_depth, MP_ARG_KW_ONLY | MP_ARG_OBJ,
          { .u_obj = mp_const_none } },
        { MP_QSTR_mono,             MP_ARG_KW_ONLY | MP_ARG_BOOL,
          { .u_bool = true } },
        { MP_QSTR_left_justified,   MP_ARG_KW_ONLY | MP_ARG_BOOL,
          { .u_bool = false } },
        { MP_QSTR_samples_signed,   MP_ARG_KW_ONLY | MP_ARG_BOOL,
          { .u_bool = true } },
        { MP_QSTR_external_clock,   MP_ARG_KW_ONLY | MP_ARG_BOOL,
          { .u_bool = false } },
        // --- past here is ours, not CircuitPython's ----------------------
        // Which I2S peripheral, as on audiobusio.I2SOut. CircuitPython picks
        // the first free one and so do we by default.
        { MP_QSTR_port,             MP_ARG_KW_ONLY | MP_ARG_INT,
          { .u_int = -1 } },
        // MCLK in bit clocks per frame, when main_clock is wired. 256 is what
        // CircuitPython's espressif port hard-codes and what an ES7210 at
        // 16 kHz locks to.
        { MP_QSTR_main_clock_fs,    MP_ARG_KW_ONLY | MP_ARG_INT,
          { .u_int = 256 } },
        // Desktop only: the wire frames `record()` reads.
        { MP_QSTR_source,           MP_ARG_KW_ONLY | MP_ARG_OBJ,
          { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    if (args[ARG_external_clock].u_bool) {
        // CircuitPython's espressif port raises the same.
        mp_raise_NotImplementedError(MP_ERROR_TEXT("external_clock"));
    }
    const mp_int_t bit_depth = args[ARG_bit_depth].u_int;
    if (!audioi2sin_depth_ok(bit_depth)) {
        mp_raise_ValueError(MP_ERROR_TEXT("bit_depth must be 8, 16, 24, or 32"));
    }
    mp_int_t out_depth = bit_depth;
    if (args[ARG_output_bit_depth].u_obj != mp_const_none) {
        out_depth = mp_obj_get_int(args[ARG_output_bit_depth].u_obj);
        if (!audioi2sin_depth_ok(out_depth)) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "output_bit_depth must be 8, 16, 24, or 32"));
        }
    }
    const mp_int_t rate = args[ARG_sample_rate].u_int;
    if (rate < 1000 || rate > 192000) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample_rate out of range"));
    }

    const int bclk = audiobusio_pin_gpio(args[ARG_bit_clock].u_obj,
        MP_QSTR_bit_clock);
    const int ws = audiobusio_pin_gpio(args[ARG_word_select].u_obj,
        MP_QSTR_word_select);
    const int din = audiobusio_pin_gpio(args[ARG_data].u_obj, MP_QSTR_data);
    const int mclk = audiobusio_pin_gpio(args[ARG_main_clock].u_obj,
        MP_QSTR_main_clock);

    const bool board = audiopump_i2s_have();
    mp_obj_t source = args[ARG_source].u_obj;
    if (source != mp_const_none) {
        if (board) {
            mp_raise_ValueError(MP_ERROR_TEXT(
                "source= is for a build with no I2S; this board records from "
                "its pins"));
        }
        mp_buffer_info_t probe;
        mp_get_buffer_raise(source, &probe, MP_BUFFER_READ);
    }
    if (board && audiopump_i2s_rx_held()) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Peripheral in use"));
    }

    audioi2sin_i2sin_obj_t *self = mp_obj_malloc_with_finaliser(
        audioi2sin_i2sin_obj_t, &audioi2sin_i2sin_type);
    memset((char *)self + sizeof(mp_obj_base_t), 0,
        sizeof(*self) - sizeof(mp_obj_base_t));
    self->mono = args[ARG_mono].u_bool;
    self->samples_signed = args[ARG_samples_signed].u_bool;
    self->bit_depth = (uint8_t)bit_depth;
    self->out_depth = (uint8_t)out_depth;
    // 24-bit samples ride in 32-bit slots, and the bus is opened at 32.
    const int wire_bits = bit_depth == 24 ? 32 : (int)bit_depth;
    self->slot_bytes = (uint8_t)(wire_bits / 8);
    self->sample_rate = (uint32_t)rate;
    self->source = source == mp_const_none ? MP_OBJ_NULL : source;

    if (board) {
        const audiopump_i2s_cfg_t cfg = {
            .port = args[ARG_port].u_int,
            .bclk = bclk,
            .ws = ws,
            .dout = -1,
            .rate = (int)rate,
            .bits = wire_bits,
            .channels = 2,
            .mclk = mclk,
            .mclk_fs = args[ARG_main_clock_fs].u_int,
            .dma_desc = 6,
            .dma_frame = audiopump_i2s_dma_frame((int)rate),
            .din = din,
            .in_port = -1,
            .in_bclk = -1,
            .in_ws = -1,
            .in_mclk = -1,
            .msb = args[ARG_left_justified].u_bool ? 1 : 0,
        };
        const uint32_t channels = self->mono ? 1 : 2;
        uint32_t ring = (uint32_t)rate * self->slot_bytes * channels
            * AUDIOI2SIN_RING_MS / 1000;
        ring = ring < 4096 ? 4096 : (ring > 65536 ? 65536 : ring);
        // Mono is the left slot, as in CircuitPython: an INMP441 with L/R
        // tied low, and the first microphone of a two-microphone ADC.
        (void)audiopump_i2s_rx_begin(&cfg, ring, self->mono ? 0 : 3);
        self->recording = true;
    }
    return MP_OBJ_FROM_PTR(self);
}

// --- deinit ----------------------------------------------------------------

static mp_obj_t audioi2sin_i2sin_deinit(mp_obj_t self_in) {
    audioi2sin_i2sin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        return mp_const_none;
    }
    if (self->recording) {
        audiopump_i2s_rx_end();
        self->recording = false;
    }
    self->source = MP_OBJ_NULL;
    self->deinited = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioi2sin_i2sin_deinit_obj,
    audioi2sin_i2sin_deinit);

static mp_obj_t audioi2sin_i2sin_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audioi2sin_i2sin_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audioi2sin_i2sin_exit_obj, 4, 4,
    audioi2sin_i2sin_exit);

// --- record ----------------------------------------------------------------

// Anything the ring dropped since we last looked becomes `overflow`.
static void audioi2sin_note_drops(audioi2sin_i2sin_obj_t *self) {
    uint32_t frames, dropped;
    audiopump_i2s_rx_counts(&frames, &dropped);
    (void)frames;
    if (dropped != self->dropped_seen) {
        self->overflow = true;
        self->dropped_seen = dropped;
    }
}

// The board: samples off the ring, already one slot or two.
static uint32_t audioi2sin_record_board(audioi2sin_i2sin_obj_t *self,
    void *buffer, uint32_t length) {
    // A ring that overflowed while nobody was recording holds a quarter of a
    // second of the past followed by a hole. Start at now instead, which is
    // what CircuitPython's recording gets: a first `record()` after a while
    // is the room as it is, and a loop that keeps up never drops anything,
    // so its recordings stay continuous call to call.
    uint32_t frames0, dropped0;
    audiopump_i2s_rx_counts(&frames0, &dropped0);
    (void)frames0;
    if (dropped0 != self->dropped_seen) {
        self->overflow = true;
        self->dropped_seen = dropped0;
        audiopump_i2s_rx_discard();
    }

    const uint32_t ch = self->mono ? 1 : 2;
    const uint32_t frame = (uint32_t)self->slot_bytes * ch;
    uint8_t scratch[512];
    const uint32_t max_frames = sizeof(scratch) / frame;
    uint32_t produced = 0;
    mp_uint_t last = mp_hal_ticks_ms();
    while (produced < length) {
        uint32_t want = (length - produced + ch - 1) / ch;
        if (want > max_frames) {
            want = max_frames;
        }
        const uint32_t got = audiopump_i2s_rx_pull(scratch, want * frame);
        if (got == 0) {
            if (mp_hal_ticks_ms() - last > AUDIOI2SIN_STALL_MS) {
                break;
            }
            // Runs the scheduler and raises a pending KeyboardInterrupt, so
            // a long recording can be stopped from the REPL.
            mp_event_wait_ms(5);
            continue;
        }
        last = mp_hal_ticks_ms();
        const uint32_t samples = got / self->slot_bytes;
        for (uint32_t i = 0; i < samples && produced < length; i++) {
            const int32_t s = audioi2sin_read(scratch + i * self->slot_bytes,
                self->slot_bytes, self->bit_depth);
            audioi2sin_store(buffer, produced++, s, self->bit_depth,
                self->out_depth, self->samples_signed);
        }
    }
    audioi2sin_note_drops(self);
    return produced;
}

// The desktop: wire frames off `source=`, two slots each, wrapping at the
// end, or silence paced off the clock.
static uint32_t audioi2sin_record_desktop(audioi2sin_i2sin_obj_t *self,
    void *buffer, uint32_t length) {
    const uint32_t ch = self->mono ? 1 : 2;
    if (self->source == MP_OBJ_NULL) {
        for (uint32_t i = 0; i < length; i++) {
            audioi2sin_store(buffer, i, 0, self->bit_depth, self->out_depth,
                self->samples_signed);
        }
        mp_hal_delay_ms((mp_uint_t)(((uint64_t)length / ch) * 1000u
            / self->sample_rate));
        return length;
    }
    mp_buffer_info_t src;
    mp_get_buffer_raise(self->source, &src, MP_BUFFER_READ);
    const size_t frame = 2u * self->slot_bytes;
    const size_t frames = src.len / frame;
    if (frames == 0) {
        return 0;
    }
    const uint8_t *p = (const uint8_t *)src.buf;
    uint32_t produced = 0;
    while (produced < length) {
        if (self->source_at + frame > frames * frame) {
            self->source_at = 0;
        }
        const uint8_t *f = p + self->source_at;
        self->source_at += frame;
        audioi2sin_store(buffer, produced++,
            audioi2sin_read(f, self->slot_bytes, self->bit_depth),
            self->bit_depth, self->out_depth, self->samples_signed);
        if (ch == 2 && produced < length) {
            audioi2sin_store(buffer, produced++,
                audioi2sin_read(f + self->slot_bytes, self->slot_bytes,
                    self->bit_depth),
                self->bit_depth, self->out_depth, self->samples_signed);
        }
    }
    return produced;
}

static mp_obj_t audioi2sin_i2sin_record(mp_obj_t self_in, mp_obj_t dest_in,
    mp_obj_t length_in) {
    audioi2sin_i2sin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioi2sin_check(self);
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
    // CircuitPython's typecode table, word for word: the buffer's type is
    // the recording's format, so a mismatch is refused rather than written
    // through at the wrong width.
    const uint8_t d = self->out_depth;
    const char tc = (char)buf.typecode;
    char want = ' ';
    if (self->samples_signed) {
        if ((d == 24 || d == 32) && tc != 'i') {
            want = 'i';
        } else if (d == 16 && tc != 'h') {
            want = 'h';
        } else if (d == 8 && tc != 'b' && buf.typecode != BYTEARRAY_TYPECODE) {
            want = 'b';
        }
    } else {
        if ((d == 24 || d == 32) && tc != 'I') {
            want = 'I';
        } else if (d == 16 && tc != 'H') {
            want = 'H';
        } else if (d == 8 && tc != 'B' && buf.typecode != BYTEARRAY_TYPECODE) {
            want = 'B';
        }
    }
    if (want != ' ') {
        mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT(
            "invalid destination buffer, must be an array of type: %c"), want);
    }
    if ((d == 24 || d == 32) && item != 4) {
        // An 'i' that is not four bytes would be written past its end.
        mp_raise_TypeError(MP_ERROR_TEXT("'i' arrays here are not 32-bit"));
    }
    const uint32_t n = self->recording
        ? audioi2sin_record_board(self, buf.buf, (uint32_t)length)
        : audioi2sin_record_desktop(self, buf.buf, (uint32_t)length);
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_3(audioi2sin_i2sin_record_obj,
    audioi2sin_i2sin_record);

// --- the properties ---------------------------------------------------------

#define AUDIOI2SIN_GETTER(name, expr) \
    static mp_obj_t audioi2sin_i2sin_get_##name(mp_obj_t self_in) { \
        audioi2sin_i2sin_obj_t *self = MP_OBJ_TO_PTR(self_in); \
        audioi2sin_check(self); \
        return (expr); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_1(audioi2sin_i2sin_get_##name##_obj, \
        audioi2sin_i2sin_get_##name); \
    MP_PROPERTY_GETTER(audioi2sin_i2sin_##name##_obj, \
        (mp_obj_t)&audioi2sin_i2sin_get_##name##_obj);

// The rate the bus was asked for. The IDF's divider lands within a fraction
// of a hertz of it, and CircuitPython reports the nominal value too.
AUDIOI2SIN_GETTER(sample_rate, mp_obj_new_int_from_uint(self->sample_rate))
// The width in the caller's buffer, which is what a consumer reads.
AUDIOI2SIN_GETTER(bits_per_sample, MP_OBJ_NEW_SMALL_INT(self->out_depth))
AUDIOI2SIN_GETTER(channel_count, MP_OBJ_NEW_SMALL_INT(self->mono ? 1 : 2))
// The width on the wire.
AUDIOI2SIN_GETTER(bit_depth, MP_OBJ_NEW_SMALL_INT(self->bit_depth))
AUDIOI2SIN_GETTER(samples_signed, mp_obj_new_bool(self->samples_signed))

// True if the ring dropped samples since this was last read; reading clears
// it. CircuitPython's espressif port always says False here, because the IDF
// drops frames without telling it. The ring counts them, so this one knows.
static mp_obj_t audioi2sin_i2sin_get_overflow(mp_obj_t self_in) {
    audioi2sin_i2sin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audioi2sin_check(self);
    if (self->recording) {
        audioi2sin_note_drops(self);
    }
    const bool was = self->overflow;
    self->overflow = false;
    return mp_obj_new_bool(was);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audioi2sin_i2sin_get_overflow_obj,
    audioi2sin_i2sin_get_overflow);
MP_PROPERTY_GETTER(audioi2sin_i2sin_overflow_obj,
    (mp_obj_t)&audioi2sin_i2sin_get_overflow_obj);

static const mp_rom_map_elem_t audioi2sin_i2sin_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audioi2sin_i2sin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audioi2sin_i2sin_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audioi2sin_i2sin_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_record), MP_ROM_PTR(&audioi2sin_i2sin_record_obj) },
    { MP_ROM_QSTR(MP_QSTR_sample_rate),
      MP_ROM_PTR(&audioi2sin_i2sin_sample_rate_obj) },
    { MP_ROM_QSTR(MP_QSTR_bits_per_sample),
      MP_ROM_PTR(&audioi2sin_i2sin_bits_per_sample_obj) },
    { MP_ROM_QSTR(MP_QSTR_channel_count),
      MP_ROM_PTR(&audioi2sin_i2sin_channel_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_bit_depth),
      MP_ROM_PTR(&audioi2sin_i2sin_bit_depth_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples_signed),
      MP_ROM_PTR(&audioi2sin_i2sin_samples_signed_obj) },
    { MP_ROM_QSTR(MP_QSTR_overflow),
      MP_ROM_PTR(&audioi2sin_i2sin_overflow_obj) },
};
static MP_DEFINE_CONST_DICT(audioi2sin_i2sin_locals,
    audioi2sin_i2sin_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audioi2sin_i2sin_type,
    MP_QSTR_I2SIn,
    MP_TYPE_FLAG_NONE,
    make_new, audioi2sin_i2sin_make_new,
    attr, cp_compat_attr,
    locals_dict, &audioi2sin_i2sin_locals
    );

static const mp_rom_map_elem_t audioi2sin_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audioi2sin) },
    { MP_ROM_QSTR(MP_QSTR_I2SIn), MP_ROM_PTR(&audioi2sin_i2sin_type) },
};
static MP_DEFINE_CONST_DICT(audioi2sin_globals, audioi2sin_globals_table);

const mp_obj_module_t audioi2sin_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioi2sin_globals,
};
MP_REGISTER_MODULE(MP_QSTR_audioi2sin, audioi2sin_module);
