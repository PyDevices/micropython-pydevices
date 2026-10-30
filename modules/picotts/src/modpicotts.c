// _picotts: the SVOX Pico text-to-speech engine, for the picotts Python
// module (py/picotts.py), which is the API to use.
//
//   engine = _picotts.Engine(ta, sg, memory)   # voice file contents, work area
//   engine.start(b"Hello.\0")                  # NUL-terminated UTF-8
//   n = engine.render(samples)                 # 16 kHz mono int16, n written
//   engine.speaking                            # False once the text is drained
//
// The engine reads the two voice buffers and its work area in place, so the
// Engine object keeps references to them. Rendering follows Adafruit's
// picotts_native.c (Adafruit_CircuitPython_PicoTTS, MIT), which this is a port
// of from a native .mpy to a user C module.
//
// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-FileCopyrightText: 2026 PyDevices
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/obj.h"
#include "py/objarray.h"
#include "py/runtime.h"

#include "picoapi.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#endif

#define VOICE ((const pico_Char *)"en-US")

// pico_getData is asked for at most this many bytes, as in the SVOX examples.
#define CHUNK_BYTES (1024)

// Pico keeps a fixed 1,000,000-byte block in its work area; the rest is
// system overhead. Smaller areas fail in pico_initialize.
#define MIN_MEMORY (1100000)

typedef struct {
    mp_obj_base_t base;
    pico_System system;
    pico_Engine engine;
    mp_obj_t memory;
    mp_obj_t ta;
    mp_obj_t sg;
    mp_obj_t text_obj;      // keeps the text alive while it renders
    const pico_Char *text;  // NUL-terminated; the NUL flushes the engine
    size_t text_len;        // including the NUL
    size_t text_pos;
    // pico_getData fails if one output item does not fit, and items are up to
    // 255 bytes. When less room than that is left in the caller's buffer,
    // output goes here first.
    int16_t carry[128];
    uint16_t carry_len;
    uint16_t carry_pos;
} picotts_engine_obj_t;

static void clear_text(picotts_engine_obj_t *self) {
    self->text_obj = mp_const_none;
    self->text = NULL;
    self->text_len = 0;
    self->text_pos = 0;
    self->carry_len = 0;
    self->carry_pos = 0;
}

static picotts_engine_obj_t *get_engine(mp_obj_t self_in) {
    picotts_engine_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->engine == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Engine deinitialized"));
    }
    return self;
}

static MP_NORETURN void raise_status(pico_System system, pico_Status status) {
    pico_Retstring msg;
    msg[0] = 0;
    if (system != NULL) {
        pico_getSystemStatusMessage(system, status, msg);
    }
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("picotts failure %d: %s"),
        (int)status, msg[0] ? (const char *)msg : "engine error");
}

// psram() -> int: bytes of PSRAM the heap can use, or -1 on a port that has no
// such idea (the desktop), where the engine always fits.
static mp_obj_t picotts_psram(void) {
    #if defined(ESP_PLATFORM)
    return mp_obj_new_int_from_uint(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    #else
    return MP_OBJ_NEW_SMALL_INT(-1);
    #endif
}
static MP_DEFINE_CONST_FUN_OBJ_0(picotts_psram_obj, picotts_psram);

// samples(n) -> memoryview: n zeroed int16 samples in one allocation.
// MicroPython has no array("h") * n, and array("h", bytes(2 * n)) briefly
// needs twice the memory, which matters for a ring of up to 960 KB.
static mp_obj_t picotts_samples(mp_obj_t n_in) {
    mp_int_t n = mp_obj_get_int(n_in);
    if (n <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("n must be > 0"));
    }
    int16_t *items = m_new0(int16_t, n);
    return mp_obj_new_memoryview('h' | MP_OBJ_ARRAY_TYPECODE_FLAG_RW, n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(picotts_samples_obj, picotts_samples);

// Engine(ta, sg, memory): ta and sg are the contents of the two voice files,
// memory the engine's work area (at least 1,100,000 bytes).
static mp_obj_t picotts_engine_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw,
    const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 3, 3, false);
    mp_buffer_info_t ta, sg, memory;
    mp_get_buffer_raise(args[0], &ta, MP_BUFFER_READ);
    mp_get_buffer_raise(args[1], &sg, MP_BUFFER_READ);
    mp_get_buffer_raise(args[2], &memory, MP_BUFFER_RW);
    if (memory.len < MIN_MEMORY) {
        mp_raise_ValueError(MP_ERROR_TEXT("memory must be at least 1100000 bytes"));
    }

    picotts_engine_obj_t *self = mp_obj_malloc(picotts_engine_obj_t, type);
    self->system = NULL;
    self->engine = NULL;
    self->ta = args[0];
    self->sg = args[1];
    self->memory = args[2];
    clear_text(self);

    pico_Status status = pico_initialize(memory.buf, memory.len, &self->system);
    if (status == PICO_OK) {
        status = pico_defineVoice(self->system, VOICE, sg.buf, ta.buf);
    }
    if (status == PICO_OK) {
        status = pico_newEngine(self->system, VOICE, &self->engine);
    }
    if (status != PICO_OK) {
        pico_System system = self->system;
        if (system != NULL) {
            pico_Retstring msg;
            msg[0] = 0;
            pico_getSystemStatusMessage(system, status, msg);
            pico_terminate(&self->system);
            self->system = NULL;
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("picotts failure %d: %s (are the voice files whole?)"),
                (int)status, msg[0] ? (const char *)msg : "engine error");
        }
        raise_status(NULL, status);
    }
    return MP_OBJ_FROM_PTR(self);
}

// deinit(): release the engine and drop the buffers. Safe to call twice.
static mp_obj_t picotts_engine_deinit(mp_obj_t self_in) {
    picotts_engine_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->engine != NULL) {
        pico_disposeEngine(self->system, &self->engine);
        self->engine = NULL;
    }
    if (self->system != NULL) {
        pico_terminate(&self->system);
        self->system = NULL;
    }
    clear_text(self);
    self->ta = self->sg = self->memory = mp_const_none;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picotts_engine_deinit_obj, picotts_engine_deinit);

// start(text): start rendering text, NUL-terminated UTF-8 bytes. Drops any
// text not yet rendered.
static mp_obj_t picotts_engine_start(mp_obj_t self_in, mp_obj_t text) {
    picotts_engine_obj_t *self = get_engine(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(text, &buf, MP_BUFFER_READ);
    if (buf.len == 0 || ((const char *)buf.buf)[buf.len - 1] != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("text must end in a NUL byte"));
    }
    pico_resetEngine(self->engine, PICO_RESET_SOFT);
    clear_text(self);
    self->text_obj = text;
    self->text = buf.buf;
    self->text_len = buf.len;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(picotts_engine_start_obj, picotts_engine_start);

// stop(): drop any text not yet rendered.
static mp_obj_t picotts_engine_stop(mp_obj_t self_in) {
    picotts_engine_obj_t *self = get_engine(self_in);
    pico_resetEngine(self->engine, PICO_RESET_SOFT);
    clear_text(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(picotts_engine_stop_obj, picotts_engine_stop);

static MP_NORETURN void fail(picotts_engine_obj_t *self, pico_Status status) {
    pico_resetEngine(self->engine, PICO_RESET_SOFT);
    clear_text(self);
    raise_status(self->system, status);
}

// render(out) -> int: samples written to out, a writable buffer of int16
// samples. Fewer than out holds only when the text is finished.
static mp_obj_t picotts_engine_render(mp_obj_t self_in, mp_obj_t out) {
    picotts_engine_obj_t *self = get_engine(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(out, &buf, MP_BUFFER_WRITE);
    int16_t *buffer = buf.buf;
    size_t length = buf.len / sizeof(int16_t);
    size_t n = 0;
    while (n < length) {
        if (self->carry_pos < self->carry_len) {
            size_t k = length - n;
            if (k > (size_t)(self->carry_len - self->carry_pos)) {
                k = self->carry_len - self->carry_pos;
            }
            memcpy(buffer + n, self->carry + self->carry_pos, k * sizeof(int16_t));
            self->carry_pos += k;
            n += k;
            continue;
        }
        if (self->text == NULL) {
            break;
        }
        if (self->text_pos < self->text_len) {
            size_t remaining = self->text_len - self->text_pos;
            pico_Int16 sent = 0;
            pico_Status status = pico_putTextUtf8(self->engine, self->text + self->text_pos,
                (pico_Int16)(remaining < 32767 ? remaining : 32767), &sent);
            if (status != PICO_OK) {
                fail(self, status);
            }
            self->text_pos += sent;
        }
        // Straight into the caller's buffer when an item surely fits, else
        // through carry.
        bool direct = (length - n) * sizeof(int16_t) >= sizeof(self->carry);
        int16_t *dest = direct ? buffer + n : self->carry;
        size_t want = sizeof(self->carry);
        if (direct) {
            want = (length - n) * sizeof(int16_t);
            if (want > CHUNK_BYTES) {
                want = CHUNK_BYTES;
            }
        }
        pico_Int16 got = 0;
        pico_Int16 type = 0;
        pico_Status status = pico_getData(self->engine, dest, (pico_Int16)want, &got, &type);
        if (direct) {
            n += got / sizeof(int16_t);
        } else {
            self->carry_len = got / sizeof(int16_t);
            self->carry_pos = 0;
        }
        if (status == PICO_STEP_IDLE && self->text_pos >= self->text_len) {
            // All text is in and the engine has nothing more: done once carry
            // drains.
            self->text = NULL;
            self->text_obj = mp_const_none;
        } else if (status != PICO_STEP_BUSY && status != PICO_STEP_IDLE) {
            fail(self, status);
        }
    }
    return MP_OBJ_NEW_SMALL_INT(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(picotts_engine_render_obj, picotts_engine_render);

static void picotts_engine_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    if (dest[0] == MP_OBJ_NULL && attr == MP_QSTR_speaking) {
        picotts_engine_obj_t *self = MP_OBJ_TO_PTR(self_in);
        dest[0] = mp_obj_new_bool(self->text != NULL || self->carry_pos < self->carry_len);
        return;
    }
    dest[1] = MP_OBJ_SENTINEL;  // look in locals_dict
}

static const mp_rom_map_elem_t picotts_engine_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&picotts_engine_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&picotts_engine_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&picotts_engine_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_render), MP_ROM_PTR(&picotts_engine_render_obj) },
};
static MP_DEFINE_CONST_DICT(picotts_engine_locals_dict, picotts_engine_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    picotts_engine_type,
    MP_QSTR_Engine,
    MP_TYPE_FLAG_NONE,
    make_new, picotts_engine_make_new,
    attr, picotts_engine_attr,
    locals_dict, &picotts_engine_locals_dict
    );

static const mp_rom_map_elem_t picotts_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__picotts) },
    { MP_ROM_QSTR(MP_QSTR_Engine), MP_ROM_PTR(&picotts_engine_type) },
    { MP_ROM_QSTR(MP_QSTR_psram), MP_ROM_PTR(&picotts_psram_obj) },
    { MP_ROM_QSTR(MP_QSTR_samples), MP_ROM_PTR(&picotts_samples_obj) },
    { MP_ROM_QSTR(MP_QSTR_SAMPLE_RATE), MP_ROM_INT(16000) },
};
static MP_DEFINE_CONST_DICT(picotts_module_globals, picotts_module_globals_table);

const mp_obj_module_t picotts_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&picotts_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR__picotts, picotts_user_cmodule);
