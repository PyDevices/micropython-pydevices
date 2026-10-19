// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// audiobusio -- CircuitPython's I2S output, with the audio pump underneath.
//
//     import audiobusio, audiocore
//     i2s = audiobusio.I2SOut(board.D1, board.D0, board.D9)
//     i2s.play(audiocore.RawSample(sine, sample_rate=8000), loop=True)
//
// That is a CircuitPython program, and on this firmware the engine under it
// is the pump: the graph is pulled on a thread of its own, pinned to the core
// the interpreter is not on, and the blocks go straight into the I2S DMA.
// Nothing about the call says so, which is the point -- our C hardware repos
// expose CircuitPython-shaped module names (displayif registers `mipidsi`,
// `spibus`, `i80bus`, `rgbmatrix`), and this is the audio one.
//
// WHY C AND NOT FROZEN PYTHON. It has to be there on a bare firmware with no
// package installed, and a frozen module is only there if the BOARD's
// manifest pulls the usermod's in -- our own esp32 builds override
// FROZEN_MANIFEST routinely, and the unix builds empty it. A C module
// registered here is in the firmware's module table whatever the manifest
// says, costs no heap to import, and is the precedent `_audioif` beside it
// already set.
//
// WHAT IT IS ON EACH BUILD. On esp32 it opens a real I2S channel. On unix and
// windows there is no I2S, and it is the same class with the same lifecycle
// writing to the driver's file sink (or to nothing), PACED off the wall clock
// so that `playing`, `pause`, `resume` and the end of a sample mean on a
// desktop what they mean on a board. CircuitPython's own unix port has no
// `audiobusio` at all; having one here is what makes the lifecycle testable
// without a board, and the constructor takes and ignores the pins, because a
// pin number is not a thing a desktop has.

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "py/runtime.h"

#include "audiocore/__init__.h"
#include "audiopump/audiopump.h"
#include "cp_compat/objproperty.h"

#include "audiopump_i2s.h"

// The pump is one global, so exactly one output can be alive. See
// "one pump, one I2S" below.
typedef struct _audiobusio_i2sout_obj_t {
    mp_obj_base_t base;
    bool deinited;
    bool opened;              // a channel is open (esp32) / the object is live
    bool playing;             // play() has been called and stop() has not
    // CircuitPython's `paused` is a flag its pause()/resume() set, and this
    // one is too -- NOT a read of whether the pump has physically reached its
    // park. unpark() clears park_req and wakes the thread; `ctx.parked` stays
    // set until that thread's spin loop notices, so asking the pump made
    // `i2s.resume(); i2s.paused` intermittently True. It passed four runs and
    // failed the fifth.
    bool paused;
    bool loop;
    audiopump_i2s_cfg_t wire; // what the constructor was given
    uint32_t rate;            // the rate the channel is currently clocked at
    const char *sink;         // desktop: the file the bytes go to, or NULL
    mp_obj_t sink_obj;        // kept alive because `sink` points into it
    mp_obj_t sample;          // what play() was handed
    mp_obj_t status;          // the pump's status bytearray
    // --- the file-backed path, and only then ------------------------------
    // A WaveFile or an MP3Decoder reads through the VFS inside its own
    // get_buffer, and the pump thread has no interpreter to do that on. So
    // the pump pulls a RING, and the interpreter fills the ring from the
    // file. That is what CircuitPython does too -- its DMA interrupt queues a
    // background callback and the fill happens on the main thread -- and the
    // trigger here is the same shape: a scheduler node, re-armed while the
    // sample plays, plus an opportunistic fill on every `playing` read, which
    // is what `while i2s.playing: pass` gives us for free.
    // --- the converted path, which is NOT the file path -------------------
    // A RawSample that is u8/u16/mono is converted on the PUMP's thread now
    // (audiodsp's `audiopump_convert_t`), so all the interpreter owns is this
    // buffer, allocated once per play(). See "the converted path" below.
    mp_obj_t conv_buf;
    uint32_t conv_len;
    bool converting;          // the pump converts every block into conv_buf
    uint8_t tail_channels;    // channel count of what the pump is pulling
    mp_obj_t ring;
    mp_obj_t ring_write;      // bound methods, resolved once: no allocation
    mp_obj_t ring_space;      // per fill, which happens a few hundred times a
    mp_obj_t ring_level;      // second.
    mp_obj_t feed_buf;        // one ring block of stereo signed 16-bit
    uint32_t feed_block;      // its length in bytes
    uint32_t feed_at;         // how much of it is filled
    const uint8_t *src;       // what is left of the last block from the file
    uint32_t src_left;
    uint8_t src_bits;
    uint8_t src_channels;
    bool src_signed;
    bool feed_done;           // the file has ended and will not be reset
} audiobusio_i2sout_obj_t;

extern const mp_obj_type_t audiobusio_i2sout_type;

// The live output, rooted because the pump and the scheduler both reach it
// from outside anything the collector can see, and because a script that
// does `audiobusio.I2SOut(...).play(sample)` without keeping the object has
// still started a thread that is holding the graph.
MP_REGISTER_ROOT_POINTER(mp_obj_t audiobusio_live);

// --- pins, exactly the way displayif takes them ---------------------------
//
// int, `machine.Pin`, or a string the port's Pin() understands. That is
// displayif's convention across mipidsi, spibus, i80bus and rgbmatrix
// (src/ports/common/mp_helpers.c), and there is no second convention in this
// workspace. CircuitPython's own `board.D1` is a `microcontroller.Pin`, which
// on a MicroPython firmware is a `machine.Pin`; a board module that hands out
// plain GPIO numbers works too and that is what our board configs do.
//
// -1 and None both mean "not wired", which is what `main_clock=None` is.

#if !MICROPY_ENABLE_SCHEDULER
#define audiobusio_arm(self) ((void)(self))
#endif

static int audiobusio_pin_gpio(mp_obj_t pin_or_int, qstr what) {
    if (pin_or_int == MP_OBJ_NULL || pin_or_int == mp_const_none) {
        return -1;
    }
    if (mp_obj_is_int(pin_or_int)) {
        return (int)mp_obj_get_int(pin_or_int);
    }
    // A machine.Pin (or a string naming one). Every port that has GPIO
    // numbers puts the number behind `Pin.id()`; asking the object rather
    // than doing pointer arithmetic into the port's pin table is the
    // portable half of displayif's helper, and it costs one call at
    // construction time.
    mp_obj_t pin = pin_or_int;
    if (mp_obj_is_str(pin_or_int)) {
        mp_obj_t machine = mp_import_name(MP_QSTR_machine, mp_const_none,
            MP_OBJ_NEW_SMALL_INT(0));
        mp_obj_t pin_cls = mp_load_attr(machine, MP_QSTR_Pin);
        pin = mp_call_function_n_kw(pin_cls, 1, 0, &pin_or_int);
    }
    mp_obj_t dest[2];
    mp_load_method_maybe(pin, MP_QSTR_id, dest);
    if (dest[0] != MP_OBJ_NULL) {
        mp_obj_t id = mp_call_method_n_kw(0, 0, dest);
        if (mp_obj_is_int(id)) {
            return (int)mp_obj_get_int(id);
        }
    }
    mp_raise_msg_varg(&mp_type_TypeError,
        MP_ERROR_TEXT("%q must be a pin number or a machine.Pin"), what);
}

// --- one pump, one I2S ----------------------------------------------------
//
// CircuitPython lets you build a second I2SOut on other pins and the port
// raises "Peripheral in use" when it runs out of I2S peripherals -- on an S3
// that is the third one. Here it is the second, because there is one pump and
// one graph, and a second output would silently orphan the first one's thread.
// The message is CircuitPython's own; the number it arrives at is ours, and it
// is written down rather than discovered.

// One DMA descriptor, in FRAMES, for a given sample rate. The channel is
// opened with six of them, so this is the granularity at which
// `i2s_channel_write` has to wait for the hardware -- and a descriptor fixed
// at 240 frames is 5 ms at 48 kHz and THIRTY at 8 kHz.
//
// At the low rates that write times out once per descriptor and loses bytes.
// Measured on the P4, two seconds a row, with the pull unchanged:
//
//     8000 Hz, 240 frames   67 timeouts, 63 432 of 64 000 bytes written
//     8000 Hz,  64 frames    0 timeouts, 64 008
//    16000 Hz, 240 frames   67 timeouts, 127 728 of 128 000
//    16000 Hz,  80 frames    0 timeouts, 128 016
//    22050 Hz, 240 frames   13 timeouts;  110 frames: 0
//    48000 Hz, 240 frames    0 timeouts
//
// This is what starved CircuitPython's own `I2SOut` docstring example, whose
// sine is at 8 kHz -- NOT the unsigned conversion it was blamed on. A signed
// 16-bit STEREO sample at 8 kHz, which is converted by nothing at all,
// starves the same 67.
//
// Sized in time it is zero at every rate, and 48 kHz still lands on the 240
// that is already proven.
static int audiobusio_dma_frame(int rate) {
    return audiopump_i2s_dma_frame(rate);
}

static audiobusio_i2sout_obj_t *audiobusio_owner(void) {
    mp_obj_t live = MP_STATE_VM(audiobusio_live);
    return live == MP_OBJ_NULL ? NULL : MP_OBJ_TO_PTR(live);
}

static void audiobusio_check(audiobusio_i2sout_obj_t *self) {
    if (self->deinited) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "Object has been deinitialized and can no longer be used. "
            "Create a new object."));
    }
}

// --- sharing the wire with a microphone ----------------------------------
//
// On a board whose codec and microphone sit on one I2S port, the output and
// the recorder are the two halves of one channel pair and share its clocks.
// Two things this output normally does to the channel would then change the
// recording under the recorder, so it stops doing them:
//
// It no longer opens a MONO channel for a mono sample. A mono slot mode would
// make the RX half mono too, so the channel stays stereo and a mono sample
// goes the converted path instead, which copies it to both slots on the
// pump's thread -- exactly what CircuitPython's output does with mono.
//
// And it does not retune the bus to a sample at another rate while the
// recorder holds the channel: that would move the microphone's rate, and the
// recording would carry on at the wrong speed with nothing to say so. It
// raises instead, naming both rates. With no recorder open it retunes as
// always, and the RX half follows.
static bool audiobusio_stereo_locked(audiobusio_i2sout_obj_t *self) {
    return self->wire.din >= 0 || audiopump_i2s_input_held();
}

// --- the feeder -----------------------------------------------------------

// Two reasons the pump cannot simply be pointed at the sample.
//
// A WaveFile or an MP3Decoder reads through the VFS inside get_buffer and the
// pump thread has no interpreter to do that on -- audiodsp refuses one at the
// door and says so.
//
// And a sample that is not already signed 16-bit has to be CONVERTED, because
// the pump writes the tail's bytes to the DMA untouched and I2S is signed.
// CircuitPython converts in the output ("Mono samples will be converted to
// stereo...", and u8/s8/u16 all the way to s16), and its own I2SOut docstring
// example is an UNSIGNED array -- so this is not an edge case, it is the
// first program anybody runs. Mono is not in this list: a mono signed sample
// opens a mono channel instead, which costs nothing and keeps the pull
// zero-copy.
//
// THE CONVERSION MOVED. It used to happen here, on the interpreter, filling
// the same ring the file path uses -- and on the P4 that cost 67 starved
// blocks in 2 s playing CircuitPython's own docstring example, because the
// ring is topped up from a scheduler node and the scheduler arrives when the
// interpreter lets it. It is a few integer operations a sample; the pump
// does it now, on its own thread, between the pull and the DMA. The ring is
// left to the one source that genuinely needs an interpreter: a file.
static bool audiobusio_file_backed(mp_obj_t sample) {
    const qstr name = mp_obj_get_type(sample)->name;
    return name == MP_QSTR_WaveFile || name == MP_QSTR_MP3Decoder;
}

static bool audiobusio_direct(mp_obj_t sample, audiosample_base_t *base) {
    if (audiobusio_file_backed(sample)) {
        return false;
    }
    return base->bits_per_sample == 16 && base->samples_signed != 0;
}

// Convert one source block into the ring's format -- signed 16-bit stereo,
// always, which is what CircuitPython's output does too ("Mono samples will
// be converted to stereo by copying value to both the left channel and the
// right channel"). Returns frames written.
static uint32_t audiobusio_convert(audiobusio_i2sout_obj_t *self,
    int16_t *out, uint32_t out_frames) {
    const uint32_t in_frame = (uint32_t)self->src_channels
        * (uint32_t)(self->src_bits / 8);
    uint32_t have = self->src_left / in_frame;
    if (have > out_frames) {
        have = out_frames;
    }
    if (have == 0) {
        return 0;
    }
    if (self->src_bits == 16 && self->src_signed && self->src_channels == 2) {
        memcpy(out, self->src, have * 4);
    } else if (self->src_bits == 16 && self->src_signed) {
        audiosample_convert_s16m_s16s(out, (const int16_t *)(const void *)self->src, have);
    } else if (self->src_bits == 16 && self->src_channels == 2) {
        audiosample_convert_u16s_s16s(out, (const uint16_t *)(const void *)self->src, have);
    } else if (self->src_bits == 16) {
        audiosample_convert_u16m_s16s(out, (const uint16_t *)(const void *)self->src, have);
    } else if (self->src_signed && self->src_channels == 2) {
        audiosample_convert_s8s_s16s(out, (const int8_t *)self->src, have);
    } else if (self->src_signed) {
        audiosample_convert_s8m_s16s(out, (const int8_t *)self->src, have);
    } else if (self->src_channels == 2) {
        audiosample_convert_u8s_s16s(out, self->src, have);
    } else {
        audiosample_convert_u8m_s16s(out, self->src, have);
    }
    self->src += have * in_frame;
    self->src_left -= have * in_frame;
    return have;
}

static void audiobusio_arm(audiobusio_i2sout_obj_t *self);

// Fill the ring from the file, as far as it will go, on this thread. Every
// call is bounded: it stops when the ring has less than a block of room, or
// when the file has ended.
static void audiobusio_feed(audiobusio_i2sout_obj_t *self) {
    // `feed_done` first, and it is not belt-and-braces: without it every later
    // call pads a fresh block of silence into the ring, the level never falls
    // to zero, and a four-tenths-of-a-second WAV plays for as long as anybody
    // keeps asking whether it is still playing. That is exactly what the gate
    // caught -- 385 024 bytes out of a 38 400-byte file.
    if (self->ring == MP_OBJ_NULL || !self->playing || self->feed_done) {
        return;
    }
    mp_buffer_info_t buf;
    mp_get_buffer_raise(self->feed_buf, &buf, MP_BUFFER_WRITE);

    for (;;) {
        const mp_int_t room = mp_obj_get_int(mp_call_function_0(self->ring_space));
        if ((uint32_t)room < self->feed_block) {
            break;
        }
        // Fill exactly one ring block, whatever the source's own block size
        // is: Ring.write takes a buffer, so handing it a partly-filled one
        // would need a memoryview per call. The remainder of a source block
        // is carried in self->src across calls instead.
        while (self->feed_at < self->feed_block) {
            if (self->src_left == 0) {
                if (self->feed_done) {
                    // Pad the tail with silence rather than a short write, so
                    // the ring's own frame alignment never moves.
                    memset((uint8_t *)buf.buf + self->feed_at, 0,
                        self->feed_block - self->feed_at);
                    self->feed_at = self->feed_block;
                    break;
                }
                uint8_t *raw = NULL;
                uint32_t len = 0;
                const audioio_get_buffer_result_t got =
                    audiosample_get_buffer(self->sample, false, 0, &raw, &len);
                if (got == GET_BUFFER_ERROR) {
                    self->feed_done = true;
                    continue;
                }
                self->src = raw;
                self->src_left = len;
                if (got == GET_BUFFER_DONE) {
                    if (self->loop) {
                        audiosample_reset_buffer(self->sample, false, 0);
                    } else {
                        self->feed_done = true;
                    }
                }
                if (len == 0) {
                    continue;
                }
            }
            const uint32_t frames = audiobusio_convert(self,
                (int16_t *)(void *)((uint8_t *)buf.buf + self->feed_at),
                (self->feed_block - self->feed_at) / 4);
            self->feed_at += frames * 4;
            if (frames == 0 && self->src_left == 0 && self->feed_done) {
                memset((uint8_t *)buf.buf + self->feed_at, 0,
                    self->feed_block - self->feed_at);
                self->feed_at = self->feed_block;
            }
        }
        mp_call_function_1(self->ring_write, self->feed_buf);
        self->feed_at = 0;
        if (self->feed_done) {
            break;
        }
    }
    audiobusio_arm(self);
}

#if MICROPY_ENABLE_SCHEDULER
#if MICROPY_SCHEDULER_STATIC_NODES
// A static node is CircuitPython's background_callback, spelt in mainline: no
// allocation, idempotent while it is queued, and a re-arm from inside the
// callback lands on the NEXT pass rather than spinning inside this one (see
// the `node != original_tail` guard in py/scheduler.c). That last property is
// what makes this safe to re-arm unconditionally.
static mp_sched_node_t audiobusio_feed_node;

static void audiobusio_feed_node_cb(mp_sched_node_t *node) {
    (void)node;
    audiobusio_i2sout_obj_t *self = audiobusio_owner();
    if (self != NULL && !self->deinited) {
        audiobusio_feed(self);
    }
}

static void audiobusio_arm(audiobusio_i2sout_obj_t *self) {
    if (self->ring != MP_OBJ_NULL && self->playing && !self->feed_done) {
        mp_sched_schedule_node(&audiobusio_feed_node, audiobusio_feed_node_cb);
    }
}
#else
static mp_obj_t audiobusio_feed_cb(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->deinited) {
        audiobusio_feed(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_feed_cb_obj, audiobusio_feed_cb);

static void audiobusio_arm(audiobusio_i2sout_obj_t *self) {
    if (self->ring != MP_OBJ_NULL && self->playing && !self->feed_done) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&audiobusio_feed_cb_obj),
            MP_OBJ_FROM_PTR(self));
    }
}
#endif
#endif

// --- the lifecycle --------------------------------------------------------

// The pump has ended on its own -- the sample ran out -- so give the thread
// back and let a later play() have the pump. Called from every read of
// `playing`, which is what `while i2s.playing: pass` polls.
static void audiobusio_reap(audiobusio_i2sout_obj_t *self) {
    // A port with no thread -- WebAssembly, or any build whose driver did not
    // bind -- advances the loop only when somebody calls service(). Without
    // this, `while i2s.playing: pass` is a hang rather than a wait: the pump
    // is live, has pulled nothing, and never will.
    if (self->playing && audiopump_c_service_mode()) {
        audiopump_c_service(16);
    }
    if (!self->playing || audiopump_is_running()) {
        return;
    }
    (void)audiopump_c_join(2000);
    self->playing = false;
    self->paused = false;
    self->sample = MP_OBJ_NULL;
    self->ring = MP_OBJ_NULL;
    self->ring_write = MP_OBJ_NULL;
    self->ring_space = MP_OBJ_NULL;
    self->ring_level = MP_OBJ_NULL;
    self->feed_buf = MP_OBJ_NULL;
}

static void audiobusio_halt(audiobusio_i2sout_obj_t *self) {
    if (!self->playing) {
        return;
    }
    audiopump_c_unpark();
    audiopump_c_stop();
    (void)audiopump_c_join(5000);
    self->playing = false;
    self->paused = false;
    self->sample = MP_OBJ_NULL;
    self->ring = MP_OBJ_NULL;
    self->ring_write = MP_OBJ_NULL;
    self->ring_space = MP_OBJ_NULL;
    self->ring_level = MP_OBJ_NULL;
    self->feed_buf = MP_OBJ_NULL;
    self->feed_done = false;
    self->feed_at = 0;
    self->src = NULL;
    self->src_left = 0;
}

static mp_obj_t audiobusio_i2sout_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_bit_clock, ARG_word_select, ARG_data, ARG_main_clock,
           ARG_left_justified, ARG_external_clock,
           ARG_port, ARG_sample_rate, ARG_main_clock_fs, ARG_sink,
           ARG_data_in };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_bit_clock,      MP_ARG_OBJ | MP_ARG_REQUIRED,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_word_select,    MP_ARG_OBJ | MP_ARG_REQUIRED,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_data,           MP_ARG_OBJ | MP_ARG_REQUIRED,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_main_clock,     MP_ARG_OBJ | MP_ARG_KW_ONLY,
          { .u_obj = mp_const_none } },
        { MP_QSTR_left_justified, MP_ARG_BOOL | MP_ARG_KW_ONLY,
          { .u_bool = false } },
        { MP_QSTR_external_clock, MP_ARG_BOOL | MP_ARG_KW_ONLY,
          { .u_bool = false } },
        // --- past here is ours, not CircuitPython's ----------------------
        // CircuitPython picks the peripheral itself (I2S_NUM_AUTO), and so do
        // we by default; `port` is here because a board can have a codec on
        // one port and a microphone on another, and the T-Embed does.
        { MP_QSTR_port,           MP_ARG_INT | MP_ARG_KW_ONLY,
          { .u_int = -1 } },
        // The bus is opened at this rate and retuned on the first play() to
        // whatever the sample asks for.
        { MP_QSTR_sample_rate,    MP_ARG_INT | MP_ARG_KW_ONLY,
          { .u_int = 48000 } },
        // How many bit clocks of MCLK per frame. 256 is what every board in
        // this workspace straps and what CircuitPython's espressif port
        // hard-codes, so it is the default -- but a codec that wants 384 has
        // nowhere else to say so, and `audiodev`'s `I2SWire` has carried the
        // number since before this module existed. Dropping it silently is
        // a half-shifted clock that sounds like a bad cable.
        { MP_QSTR_main_clock_fs,  MP_ARG_INT | MP_ARG_KW_ONLY,
          { .u_int = 256 } },
        // Desktop only: the file the blocks are written to.
        { MP_QSTR_sink,           MP_ARG_OBJ | MP_ARG_KW_ONLY,
          { .u_obj = mp_const_none } },
        // The microphone's data pin, on a board whose codec and microphone
        // share this output's clocks. The channel is then opened as a TX/RX
        // pair, so `_audioif.rx_open()` can record while this plays; see
        // "sharing the wire with a microphone" below.
        { MP_QSTR_data_in,        MP_ARG_OBJ | MP_ARG_KW_ONLY,
          { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    if (args[ARG_external_clock].u_bool) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("external_clock"));
    }
    if (args[ARG_left_justified].u_bool) {
        // The pump's channel is Philips-mode. Saying so is better than a
        // silently half-shifted bus.
        mp_raise_NotImplementedError(MP_ERROR_TEXT("left_justified"));
    }
    if (audiobusio_owner() != NULL) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("Peripheral in use"));
    }

    audiobusio_i2sout_obj_t *self = mp_obj_malloc_with_finaliser(
        audiobusio_i2sout_obj_t, &audiobusio_i2sout_type);
    memset((char *)self + sizeof(mp_obj_base_t), 0,
        sizeof(*self) - sizeof(mp_obj_base_t));

    self->wire.port = args[ARG_port].u_int;
    self->wire.bclk = audiobusio_pin_gpio(args[ARG_bit_clock].u_obj,
        MP_QSTR_bit_clock);
    self->wire.ws = audiobusio_pin_gpio(args[ARG_word_select].u_obj,
        MP_QSTR_word_select);
    self->wire.dout = audiobusio_pin_gpio(args[ARG_data].u_obj, MP_QSTR_data);
    self->wire.mclk = audiobusio_pin_gpio(args[ARG_main_clock].u_obj,
        MP_QSTR_main_clock);
    self->wire.rate = args[ARG_sample_rate].u_int;
    self->wire.bits = 16;
    self->wire.channels = 2;
    self->wire.mclk_fs = args[ARG_main_clock_fs].u_int;
    self->wire.dma_desc = 6;
    self->wire.dma_frame = audiobusio_dma_frame(self->wire.rate);
    self->wire.din = audiobusio_pin_gpio(args[ARG_data_in].u_obj,
        MP_QSTR_data_in);
    self->wire.in_port = -1;
    self->wire.in_bclk = -1;
    self->wire.in_ws = -1;
    self->wire.in_mclk = -1;
    self->rate = (uint32_t)self->wire.rate;

    if (args[ARG_sink].u_obj != mp_const_none) {
        self->sink_obj = args[ARG_sink].u_obj;
        self->sink = mp_obj_str_get_str(args[ARG_sink].u_obj);
    }

    if (audiopump_i2s_have()) {
        (void)audiopump_i2s_open(&self->wire);
    }
    self->opened = true;
    // by_ref over zeroed memory: mp_obj_new_bytearray() memcpy()s from its
    // second argument and NULL is not a source.
    self->status = mp_obj_new_bytearray_by_ref(AUDIOPUMP_STATUS_BYTES,
        m_new0(uint8_t, AUDIOPUMP_STATUS_BYTES));
    MP_STATE_VM(audiobusio_live) = MP_OBJ_FROM_PTR(self);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t audiobusio_i2sout_deinit(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->deinited) {
        return mp_const_none;
    }
    audiobusio_halt(self);
    if (self->opened && audiopump_i2s_have()) {
        audiopump_i2s_shutdown();
    }
    self->opened = false;
    self->deinited = true;
    self->status = MP_OBJ_NULL;
    if (audiobusio_owner() == self) {
        MP_STATE_VM(audiobusio_live) = MP_OBJ_NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_deinit_obj,
    audiobusio_i2sout_deinit);

static mp_obj_t audiobusio_i2sout_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return audiobusio_i2sout_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(audiobusio_i2sout_exit_obj, 4, 4,
    audiobusio_i2sout_exit);

// --- play -----------------------------------------------------------------

static void audiobusio_start_direct(audiobusio_i2sout_obj_t *self,
    mp_obj_t sample, bool loop) {
    const bool board = audiopump_i2s_have();
    mp_obj_t sink = board ? mp_const_true
        : (self->sink != NULL ? self->sink_obj : mp_const_none);
    // On a desktop the sink is not a clock, so the pump is paced off the wall
    // clock instead; on a board the DMA is the pace and pacing twice would
    // be a stall. `core = -1` lets the driver pin the task itself.
    (void)audiopump_c_spawn(sample, self->status, UINT64_MAX, sink, loop,
        !board, -1, 200, NULL);
}

// The converted path: the same spawn, plus a scratch the pump converts each
// block into. Nothing on the interpreter afterwards -- no ring, no scheduler
// node, no fill on every `playing` read.
static void audiobusio_start_converted(audiobusio_i2sout_obj_t *self,
    mp_obj_t sample, bool loop, audiosample_base_t *base) {
    const uint32_t in_frame = (uint32_t)base->channel_count
        * (uint32_t)(base->bits_per_sample / 8);
    const uint32_t frames = in_frame
        ? (uint32_t)base->max_buffer_length / in_frame : 0;
    if (frames == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("sample has no frames"));
    }
    // Stereo signed 16-bit, and twice what this sample needs: a `retarget()`
    // keeps the conversion, and the tail it swaps in -- a root Mixer that
    // audiodev builds when a second client arrives -- is sized at twice its
    // first voice's block. The scratch cannot be resized under a running
    // pump, so it is sized for that here rather than refused there.
    const uint32_t need = frames * 4 * 2;
    if (self->conv_buf == MP_OBJ_NULL || self->conv_len < need) {
        self->conv_buf = mp_obj_new_bytearray_by_ref(need,
            m_new0(uint8_t, need));
        self->conv_len = need;
    }
    self->sample = sample;
    const audiopump_convert_t cfg = {
        .scratch = self->conv_buf,
        .bits = (uint8_t)base->bits_per_sample,
        .channels = base->channel_count,
        .is_signed = base->samples_signed != 0,
    };
    const bool board = audiopump_i2s_have();
    mp_obj_t sink = board ? mp_const_true
        : (self->sink != NULL ? self->sink_obj : mp_const_none);
    (void)audiopump_c_spawn(sample, self->status, UINT64_MAX, sink, loop,
        !board, -1, 200, &cfg);
}

static void audiobusio_start_fed(audiobusio_i2sout_obj_t *self,
    mp_obj_t sample, bool loop, audiosample_base_t *base) {
    // The ring the pump pulls, and the buffer the interpreter fills it from.
    // Stereo 16-bit signed whatever the file is: the converters above make it
    // so, exactly as CircuitPython's output does.
    mp_obj_t audiopump_mod = mp_import_name(MP_QSTR_audiopump, mp_const_none,
        MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t ring_type = mp_load_attr(audiopump_mod, MP_QSTR_Ring);
    const mp_int_t frames = 512;
    mp_obj_t kw[8] = {
        MP_OBJ_NEW_QSTR(MP_QSTR_sample_rate),
        mp_obj_new_int((mp_int_t)base->sample_rate),
        MP_OBJ_NEW_QSTR(MP_QSTR_channel_count), MP_OBJ_NEW_SMALL_INT(2),
        MP_OBJ_NEW_QSTR(MP_QSTR_frames), MP_OBJ_NEW_SMALL_INT(frames),
        MP_OBJ_NEW_QSTR(MP_QSTR_capacity), MP_OBJ_NEW_SMALL_INT(6),
    };
    self->ring = mp_call_function_n_kw(ring_type, 0, 4, kw);
    self->ring_write = mp_load_attr(self->ring, MP_QSTR_write);
    self->ring_space = mp_load_attr(self->ring, MP_QSTR_space);
    self->ring_level = mp_load_attr(self->ring, MP_QSTR_level);
    self->feed_block = (uint32_t)frames * 4;
    self->feed_buf = mp_obj_new_bytearray_by_ref(self->feed_block,
        m_new0(uint8_t, self->feed_block));
    self->feed_at = 0;
    self->feed_done = false;
    self->src = NULL;
    self->src_left = 0;
    self->src_bits = (uint8_t)base->bits_per_sample;
    self->src_channels = base->channel_count;
    self->src_signed = base->samples_signed != 0;
    self->sample = sample;
    self->loop = loop;
    self->playing = true;          // so the first feed will run
    audiobusio_feed(self);         // prime it before a byte is clocked
    self->playing = false;
    audiobusio_start_direct(self, self->ring, false);
}

static mp_obj_t audiobusio_i2sout_play(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample, MP_ARG_OBJ | MP_ARG_REQUIRED,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_loop,   MP_ARG_BOOL | MP_ARG_KW_ONLY, { .u_bool = false } },
    };
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    audiobusio_check(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    mp_obj_t sample = args[ARG_sample].u_obj;
    audiosample_base_t *base = audiosample_check(sample);   // raises TypeError

    // Before anything stops: a refusal must leave what is playing playing.
    if (audiopump_i2s_have() && audiopump_i2s_input_held()
        && base->sample_rate != self->rate) {
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT(
            "a microphone is recording at %u Hz on this I2S channel; a "
            "%u Hz sample would change its rate"),
            (unsigned)self->rate, (unsigned)base->sample_rate);
    }

    // CircuitPython stops whatever was playing. So do we, and the join is
    // what makes the pump free for the new graph.
    audiobusio_halt(self);
    audiobusio_reap(self);

    // CircuitPython retunes the bus to the sample's rate on every play, and
    // the amplifier hears the difference. The channel is idle at this exact
    // moment -- the pump has been joined -- which is the only moment it is
    // safe to reopen. The slot count moves with it: a mono signed sample
    // opens a mono channel rather than being copied into a stereo one.
    const bool direct = audiobusio_direct(sample, base)
        && !(audiobusio_stereo_locked(self) && base->channel_count != 2);
    const int want_channels = direct ? (int)base->channel_count : 2;
    if (audiopump_i2s_have()
        && (base->sample_rate != self->rate
            || want_channels != self->wire.channels)) {
        audiopump_i2s_shutdown();
        self->wire.rate = (int)base->sample_rate;
        self->wire.channels = want_channels;
        self->wire.dma_frame = audiobusio_dma_frame(self->wire.rate);
        (void)audiopump_i2s_open(&self->wire);
        self->rate = base->sample_rate;
    }

    audiosample_reset_buffer(sample, false, 0);
    self->loop = args[ARG_loop].u_bool;
    // `starved()` counts THIS performance. Whatever the channel clocked while
    // nothing was playing -- between a stop and this play, most of all --
    // is silence somebody asked for, and charging it here would make the
    // number grow with how long the board sat idle.
    audiopump_i2s_starved_reset();
    self->converting = false;
    self->tail_channels = base->channel_count;
    if (direct) {
        self->sample = sample;
        audiobusio_start_direct(self, sample, self->loop);
    } else if (!audiobusio_file_backed(sample)) {
        audiobusio_start_converted(self, sample, self->loop, base);
        self->converting = true;
    } else {
        audiobusio_start_fed(self, sample, self->loop, base);
        self->tail_channels = 2;      // the pump pulls the stereo ring
    }
    self->playing = true;
    #if MICROPY_ENABLE_SCHEDULER
    audiobusio_arm(self);
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiobusio_i2sout_play_obj, 1,
    audiobusio_i2sout_play);

static mp_obj_t audiobusio_i2sout_stop(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    audiobusio_halt(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_stop_obj,
    audiobusio_i2sout_stop);

// --- retarget: OURS, not CircuitPython's ----------------------------------
//
// CircuitPython has no way to change what an output is playing without
// stopping it, because nothing above it ever needs one: one `I2SOut`, one
// graph, and `play()` again is a fine way to start a different sound.
//
// `audiodev` needs one. It is the policy layer over this class -- two
// clients, a root `audiomixer.Mixer` built for them, volume, the codec
// session -- and every time a client arrives or leaves it has a NEW tail for
// the same output. Going through `play()` there is a `stop()` + `join()` +
// a fresh channel between two blocks of audio that a listener is in the
// middle of, on every kit change and every note a second voice starts.
//
// So: one method, one word, listed beside `starved()` as ours. It is the
// pump's own `retarget` with the object rooted here, which is the part a
// caller reaching for `audiopump.retarget()` behind this class's back would
// get wrong -- `self->sample` would still hold the OLD graph and the new one
// would be unrooted while a thread pulls it.
//
// `loop` travels with the tail rather than staying as `play()` set it: a
// root Mixer replaced by the one client still sounding is a different thing
// to loop. Leaving it behind is exactly how a looping client left alone on a
// live pump stopped at the end of its first lap.
static mp_obj_t audiobusio_i2sout_retarget(size_t n_args,
    const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sample, ARG_loop };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sample, MP_ARG_OBJ | MP_ARG_REQUIRED,
          { .u_obj = MP_OBJ_NULL } },
        { MP_QSTR_loop,   MP_ARG_BOOL | MP_ARG_KW_ONLY, { .u_bool = false } },
    };
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    audiobusio_check(self);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed),
        allowed, args);

    if (!self->playing) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Not playing"));
    }
    mp_obj_t sample = args[ARG_sample].u_obj;
    audiosample_base_t *base = audiosample_check(sample);   // raises TypeError
    // The conversion scratch and the feeder's ring were both sized for the
    // sample `play()` was handed, and neither can be resized under a running
    // pump. A tail that needs either is a `play()`, not a swap -- said out
    // loud rather than silently clipped or half-converted.
    if (!audiobusio_direct(sample, base)) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "retarget takes a signed 16-bit sample; call play() instead"));
    }
    if (audiopump_i2s_have() && base->sample_rate != self->rate) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "retarget cannot retune the bus; call play() instead"));
    }
    // The pump writes what it pulls (or what it converts it to) straight into
    // a channel whose slot count was set for the old tail. A tail with another
    // channel count would play at twice or half speed, so it is refused.
    if (base->channel_count != self->tail_channels) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "retarget cannot change the channel count; call play() instead"));
    }
    if (self->converting && base->channel_count
        && (base->max_buffer_length / (2u * base->channel_count)) * 4u
            > self->conv_len) {
        mp_raise_ValueError(MP_ERROR_TEXT(
            "retarget: this sample's block is too big for the conversion "
            "buffer; call play() instead"));
    }
    audiopump_c_retarget(sample, args[ARG_loop].u_bool ? 1 : 0);
    self->sample = sample;
    self->loop = args[ARG_loop].u_bool;
    // Whatever was feeding the old tail is not feeding this one. Dropping the
    // ring here is what stops the scheduler node topping up a ring nothing
    // pulls any more.
    self->ring = MP_OBJ_NULL;
    self->ring_write = MP_OBJ_NULL;
    self->ring_space = MP_OBJ_NULL;
    self->ring_level = MP_OBJ_NULL;
    self->feed_buf = MP_OBJ_NULL;
    self->feed_done = false;
    self->feed_at = 0;
    self->src = NULL;
    self->src_left = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(audiobusio_i2sout_retarget_obj, 2,
    audiobusio_i2sout_retarget);

// The pump's status block, as the bytearray the loop writes into. Ours, and
// the reason it is here rather than behind `audiopump.STATUS_*` constants is
// that this object owns the buffer: a caller that spawned through `play()`
// has no other way to reach the two words that say WHY the pump stopped, and
// `audiodev` turns those into the one sentence an app prints.
static mp_obj_t audiobusio_i2sout_get_status(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    return self->status;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_get_status_obj,
    audiobusio_i2sout_get_status);
MP_PROPERTY_GETTER(audiobusio_i2sout_status_obj,
    (mp_obj_t)&audiobusio_i2sout_get_status_obj);

static mp_obj_t audiobusio_i2sout_get_playing(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    // `while i2s.playing: pass` is the CircuitPython idiom, so this is the
    // one call a player is guaranteed to make often. Both housekeeping jobs
    // ride on it: top the ring up, and give the thread back once the sample
    // has run out.
    if (self->ring != MP_OBJ_NULL) {
        audiobusio_feed(self);
        // The file has ended and the ring has handed the last of it to the
        // DMA. Writes and reads are both one ring block, so the level lands
        // exactly on zero rather than near it.
        if (self->feed_done && self->playing
            && mp_obj_get_int(mp_call_function_0(self->ring_level)) == 0) {
            audiobusio_halt(self);
        }
    }
    audiobusio_reap(self);
    return mp_obj_new_bool(self->playing);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_get_playing_obj,
    audiobusio_i2sout_get_playing);
MP_PROPERTY_GETTER(audiobusio_i2sout_playing_obj,
    (mp_obj_t)&audiobusio_i2sout_get_playing_obj);

static mp_obj_t audiobusio_i2sout_pause(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    if (!self->playing) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("Not playing"));
    }
    // The pump parks at a block boundary and stops handing the DMA anything.
    // The channel keeps clocking, and because it was opened with auto_clear
    // it clocks zeros -- so a pause is silence with the bus still up, where
    // CircuitPython disables the channel outright. See "what differs".
    audiopump_i2s_starved_hold();
    (void)audiopump_c_park(200000);
    self->paused = true;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_pause_obj,
    audiobusio_i2sout_pause);

static mp_obj_t audiobusio_i2sout_resume(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    if (self->paused) {
        self->paused = false;
        // A pause clocks zeros deliberately and was not counted. Count
        // again from the pump's first write after it, keeping the total.
        audiopump_i2s_starved_resume();
        audiopump_c_unpark();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_resume_obj,
    audiobusio_i2sout_resume);

static mp_obj_t audiobusio_i2sout_get_paused(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    return mp_obj_new_bool(self->playing && self->paused);
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_get_paused_obj,
    audiobusio_i2sout_get_paused);
MP_PROPERTY_GETTER(audiobusio_i2sout_paused_obj,
    (mp_obj_t)&audiobusio_i2sout_get_paused_obj);

// Not CircuitPython's, and deliberately one word: what the pump made of it.
// A player that wants to know whether a block was ever late has nowhere else
// to look, and printing the whole status bytearray is what every probe in
// this spike had to do before.
//
// **Bytes of silence.** This used to return the engine's SINK_TIMEOUTS --
// how many times the sink refused a block -- and that number had never been
// seen to move, on either chip, including through a stall that plainly
// starved the wire: 32 kB written to flash while a 48 kHz loop played, the
// two-second leg taking 7.3 s and clocking 40 283 frames a second instead of
// 47 928, and `starved()` reading 0 before and 0 after (audioif#8). It could
// not have read anything else. A refusal needs the pump running and the DMA
// full, which is the one situation that is NOT an underrun.
//
// It now reports audio that should have played and did not, in bytes --
// divide by the frame size and the rate for milliseconds -- since `play()`.
// `_audioif` counts it as events in the DMA's interrupt: a descriptor started
// with nothing written into it, or the DMA standing still. So it reads 0 on a
// clean playback and never goes down; a pause is not counted, and the count
// survives one. The witnesses, and the measurements behind each, are written
// down beside their state in `_audioif.c`.
//
// `sink_timeouts()` below keeps the old number under its real name, because
// it is not worthless -- it says the pump produced a block the DMA had no
// room for, which is the opposite failure and worth telling apart.
static mp_obj_t audiobusio_i2sout_starved(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    return mp_obj_new_int_from_ull(audiopump_i2s_starved());
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_starved_obj,
    audiobusio_i2sout_starved);

// Blocks the sink refused: the pump had audio ready and the DMA had no room
// for it. The opposite of `starved()` and a different fault -- this one means
// the pump is ahead, that one means the wire ran dry.
static mp_obj_t audiobusio_i2sout_sink_timeouts(mp_obj_t self_in) {
    audiobusio_i2sout_obj_t *self = MP_OBJ_TO_PTR(self_in);
    audiobusio_check(self);
    return mp_obj_new_int_from_ull(
        audiopump_c_status(AUDIOPUMP_STATUS_SINK_TIMEOUTS));
}
static MP_DEFINE_CONST_FUN_OBJ_1(audiobusio_i2sout_sink_timeouts_obj,
    audiobusio_i2sout_sink_timeouts);

static const mp_rom_map_elem_t audiobusio_i2sout_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&audiobusio_i2sout_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&audiobusio_i2sout_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&audiobusio_i2sout_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_play), MP_ROM_PTR(&audiobusio_i2sout_play_obj) },
    { MP_ROM_QSTR(MP_QSTR_retarget),
      MP_ROM_PTR(&audiobusio_i2sout_retarget_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&audiobusio_i2sout_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_pause), MP_ROM_PTR(&audiobusio_i2sout_pause_obj) },
    { MP_ROM_QSTR(MP_QSTR_resume), MP_ROM_PTR(&audiobusio_i2sout_resume_obj) },
    { MP_ROM_QSTR(MP_QSTR_playing), MP_ROM_PTR(&audiobusio_i2sout_playing_obj) },
    { MP_ROM_QSTR(MP_QSTR_paused), MP_ROM_PTR(&audiobusio_i2sout_paused_obj) },
    { MP_ROM_QSTR(MP_QSTR_starved), MP_ROM_PTR(&audiobusio_i2sout_starved_obj) },
    { MP_ROM_QSTR(MP_QSTR_sink_timeouts),
      MP_ROM_PTR(&audiobusio_i2sout_sink_timeouts_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&audiobusio_i2sout_status_obj) },
};
static MP_DEFINE_CONST_DICT(audiobusio_i2sout_locals,
    audiobusio_i2sout_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    audiobusio_i2sout_type,
    MP_QSTR_I2SOut,
    MP_TYPE_FLAG_NONE,
    make_new, audiobusio_i2sout_make_new,
    attr, cp_compat_attr,
    locals_dict, &audiobusio_i2sout_locals
    );

// --- the input half, named and refused ------------------------------------
//
// CircuitPython 10.3.0 puts PDM microphones in `audiobusio.PDMIn` and I2S
// capture in a module of its own, `audioi2sin.I2SIn` -- it is NOT in
// audiobusio, which is why this module has no I2SIn either. Neither is built
// here yet: the pump already has an input node (`_audioif.Input`), and what an
// `I2SIn` would be is that node plus a channel opened for RX. PDMIn is a
// different peripheral and a different decimation filter, and nothing in this
// workspace has one wired.
//
// The name exists and says so, which is displayif's precedent for a bus a
// port cannot do (src/ports/common/notimpl/): a NotImplementedError naming the
// class, rather than an AttributeError that reads like a typo.

static mp_obj_t audiobusio_pdmin_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_raise_NotImplementedError(MP_ERROR_TEXT(
        "PDMIn: no PDM peripheral is wired on this board"));
}

static MP_DEFINE_CONST_OBJ_TYPE(
    audiobusio_pdmin_type,
    MP_QSTR_PDMIn,
    MP_TYPE_FLAG_NONE,
    make_new, audiobusio_pdmin_make_new
    );

static const mp_rom_map_elem_t audiobusio_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_audiobusio) },
    { MP_ROM_QSTR(MP_QSTR_I2SOut), MP_ROM_PTR(&audiobusio_i2sout_type) },
    { MP_ROM_QSTR(MP_QSTR_PDMIn), MP_ROM_PTR(&audiobusio_pdmin_type) },
};
static MP_DEFINE_CONST_DICT(audiobusio_globals, audiobusio_globals_table);

const mp_obj_module_t audiobusio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audiobusio_globals,
};
MP_REGISTER_MODULE(MP_QSTR_audiobusio, audiobusio_module);
