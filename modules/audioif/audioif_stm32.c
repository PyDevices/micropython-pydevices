// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The audio pump's output on an STM32H7's DAC: the Arduino GIGA R1's headphone
// jack, whose left is DAC1's channel 2 on PA5 (DAC1, A13) and whose right is
// channel 1 on PA4 (DAC0, A12), per Arduino's schematic. Each sample goes to
// the pin audioio.AudioOut names for its side.
//
// The stm32 port has no threads to give the pump, so the engine runs in its
// service mode and this driver is what calls service(). The DAC plays a ring
// of 32-bit words, both channels at once through DHR12RD, fed by a circular
// DMA and paced by TIM6. A soft timer looks at the DMA every few milliseconds
// from the PendSV interrupt; when a block's worth of the ring has been played
// it schedules a callback, and that callback, on the interpreter, runs the
// pump until the ring is full again. The pump therefore renders between
// bytecodes, and while the interpreter waits in sleep() or a blocking read,
// never inside an interrupt: a graph may allocate and may raise, and neither
// is allowed there. What that costs is that a C call that holds the
// interpreter longer than the ring (about 93 ms at any rate) underruns, and the
// ring plays silence until the pump catches up.
//
// The hook table is in this file, with MP_REGISTER_MODULE, so that a static
// archive can't let the engine's weak default win (audiodsp's
// src/shared/audiodsp_port.h says why). _audioif.c compiles to no driver on
// this port, and the functions audiobusio.c asks of an I2S channel are
// answered here, for the DAC.

#include <string.h>

#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "shared/runtime/softtimer.h"

#include "dma.h"
#include "pin.h"
#include "timer.h"

#include "audiocore/__init__.h"
#include "audiopump/audiopump.h"
#include "shared/audiodsp_port.h"

#include "audiopump_i2s.h"

#if defined(AUDIOIF_DRIVER_STM32)

// The ring holds about 93 ms at whatever rate the DAC opens at, or what
// audioio.AudioOut's buffer_ms= asks for, up to AUDIOIF_RING_MAX frames (278
// ms at 44.1 kHz, 256 ms at 48 kHz): how long a C call may hold the
// interpreter before the DAC underruns, and how far behind the sound is. A
// word per frame.
#define AUDIOIF_RING_MS (93)
#define AUDIOIF_RING_MAX (12288)
#define AUDIOIF_RING_MIN (512)
// How often the soft timer looks at the DMA.
#define AUDIOIF_KICK_MS (4)
// DAC midscale, the silence of an unsigned 12-bit output, on both channels.
#define AUDIOIF_SILENCE (0x08000800u)

// In .bss, which the H7 boards' linker scripts put in AXI SRAM, where DMA1
// can read it; DMA1 can't reach the DTCM, and audiopump_i2s_open() refuses a
// board that puts .bss there. The CPU writes the ring through the data cache,
// so every write is cleaned out to memory before the DMA can get to it.
static uint32_t audioif_ring[AUDIOIF_RING_MAX] __attribute__((aligned(32)));
// Frames of it in use, set on each open from the rate; a multiple of eight,
// so the ring is whole cache lines.
static uint32_t audioif_ring_frames = 2048;
#define AUDIOIF_RING_FRAMES (audioif_ring_frames)

// The DMA stream that dma.c gives DAC1 channel 1 on an H7 (dma_DAC_1_TX).
#define AUDIOIF_DMA_STREAM DMA1_Stream5

// Counted in frames since the channel opened. `played` is the DMA's read
// position, unwrapped; `written` is how far the pump has filled. The pump may
// be at most a ring ahead of the DMA.
static volatile uint64_t audioif_played;
static volatile uint64_t audioif_written;
static uint32_t audioif_last_pos;
// Frames the DMA played that the pump hadn't written: the silence a listener
// heard, since the performance began.
static volatile uint64_t audioif_starved_frames;
static bool audioif_starved_counting;
// Count from the pump's next write, not from play(): the ring is silence
// until the first kick fills it, and nobody asked for that to be audio.
static bool audioif_starved_pending;

static bool audioif_open;
static audiopump_i2s_cfg_t audioif_live;
static uint8_t audioif_channels;   // the stream's: 1 or 2
// True when the left channel's pin is PA5, DAC channel 2: DHR12RD holds
// channel 1 in its low half, so each frame is packed the other way round.
static bool audioif_swap;
// The largest block the pump has written, in frames: the room a kick waits
// for, so a block is never cut short.
static uint32_t audioif_block = 512;

static soft_timer_entry_t audioif_kick_timer;
static mp_sched_node_t audioif_kick_node;
static uint32_t audioif_service_errors;

// --- the ring --------------------------------------------------------------

static void audioif_clean(uint32_t from, uint32_t frames) {
    // Cache lines are 32 bytes, eight frames.
    uintptr_t a = (uintptr_t)&audioif_ring[from] & ~(uintptr_t)31;
    uintptr_t b = (uintptr_t)&audioif_ring[from + frames];
    SCB_CleanDCache_by_Addr((uint32_t *)a, (int32_t)(b - a));
}

// Fill [from, to) of the ring with silence, in absolute frames.
static void audioif_silence(uint64_t from, uint64_t to) {
    while (from < to) {
        uint32_t at = (uint32_t)(from % AUDIOIF_RING_FRAMES);
        uint32_t n = AUDIOIF_RING_FRAMES - at;
        if ((uint64_t)n > to - from) {
            n = (uint32_t)(to - from);
        }
        for (uint32_t i = 0; i < n; i++) {
            audioif_ring[at + i] = AUDIOIF_SILENCE;
        }
        audioif_clean(at, n);
        from += n;
    }
}

// Bring `played` up to the DMA, and make sure that anything it reaches next
// that the pump hasn't written plays as silence rather than the ring's last
// lap. Called from the soft timer and from the interpreter, so it runs with
// interrupts off.
static void audioif_poll(void) {
    if (!audioif_open) {
        return;
    }
    mp_uint_t state = MICROPY_BEGIN_ATOMIC_SECTION();
    uint32_t pos = AUDIOIF_RING_FRAMES - AUDIOIF_DMA_STREAM->NDTR;
    if (pos >= AUDIOIF_RING_FRAMES) {
        pos = 0;
    }
    uint32_t moved = (pos + AUDIOIF_RING_FRAMES - audioif_last_pos) % AUDIOIF_RING_FRAMES;
    audioif_last_pos = pos;
    uint64_t was = audioif_played;
    audioif_played = was + moved;
    if (audioif_written < audioif_played) {
        // Starved: the DMA ran past everything the pump wrote.
        if (audioif_starved_counting) {
            audioif_starved_frames += audioif_played - audioif_written;
        }
        audioif_written = audioif_played;
    }
    // The frames just played come round again a lap later. Those the pump
    // hasn't already written for that lap become silence.
    uint64_t from = was + AUDIOIF_RING_FRAMES;
    if (from < audioif_written) {
        from = audioif_written;
    }
    audioif_silence(from, audioif_played + AUDIOIF_RING_FRAMES);
    MICROPY_END_ATOMIC_SECTION(state);
}

static uint32_t audioif_space(void) {
    return (uint32_t)(AUDIOIF_RING_FRAMES - (audioif_written - audioif_played));
}

// --- the kick: the pump runs here, on the interpreter ---------------------

// Run the pump only while the ring has a block's room, so the sink write
// never waits for the DMA. Bounded, so a pump that stopped writing can't hold
// the interpreter.
void audiopump_i2s_service(void) {
    for (int i = 0; i < 32; i++) {
        audioif_poll();
        if (audioif_space() < audioif_block) {
            break;
        }
        uint64_t before = audioif_written;
        audiopump_c_service(1);
        if (audioif_written == before) {
            break;
        }
    }
}

static void audioif_kick_cb(mp_sched_node_t *node) {
    (void)node;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        audiopump_i2s_service();
        nlr_pop();
    } else {
        audioif_service_errors++;
    }
}

static void audioif_kick_timer_cb(soft_timer_entry_t *self) {
    (void)self;
    audioif_poll();
    if (audioif_space() >= audioif_block && audiopump_c_service_mode()) {
        mp_sched_schedule_node(&audioif_kick_node, audioif_kick_cb);
    }
}

// --- the hardware ----------------------------------------------------------

static const machine_pin_obj_t *audioif_dac_pin(int channel) {
    return channel == 1 ? pin_A4 : pin_A5;
}

// The DAC channel a pin id (port * 16 + pin) is: 1 for PA4, 2 for PA5.
static int audioif_dac_channel(int id) {
    if (id == 4) {
        return 1;
    }
    if (id == 5) {
        return 2;
    }
    return 0;
}

static void audioif_hw_stop(void) {
    soft_timer_remove(&audioif_kick_timer);
    AUDIOIF_DMA_STREAM->CR &= ~DMA_SxCR_EN;
    while (AUDIOIF_DMA_STREAM->CR & DMA_SxCR_EN) {
    }
    DAC1->CR &= ~(DAC_CR_DMAEN1 | DAC_CR_DMAEN2);
    // Leave the outputs at midscale, where the speaker rests.
    DAC1->DHR12RD = AUDIOIF_SILENCE;
    DAC1->SWTRIGR = DAC_SWTRIGR_SWTRIG1 | DAC_SWTRIGR_SWTRIG2;
    TIM6->CR1 &= ~TIM_CR1_CEN;
    audioif_open = false;
}

static void audioif_hw_start(int left, int right, uint32_t rate) {
    __HAL_RCC_DAC12_CLK_ENABLE();
    mp_hal_pin_config(audioif_dac_pin(left), MP_HAL_PIN_MODE_ANALOG, MP_HAL_PIN_PULL_NONE, 0);
    if (right) {
        mp_hal_pin_config(audioif_dac_pin(right), MP_HAL_PIN_MODE_ANALOG, MP_HAL_PIN_PULL_NONE, 0);
    }

    for (uint32_t i = 0; i < AUDIOIF_RING_FRAMES; i++) {
        audioif_ring[i] = AUDIOIF_SILENCE;
    }
    audioif_clean(0, AUDIOIF_RING_FRAMES);
    audioif_played = 0;
    audioif_written = 0;
    audioif_last_pos = 0;

    // Both channels, buffered onto their pins, triggered together by TIM6;
    // channel 1's request moves one DHR12RD word, both channels' samples, per
    // trigger.
    DAC1->CR = 0;
    DAC1->MCR &= ~(DAC_MCR_MODE1 | DAC_MCR_MODE2);
    DAC1->CR = DAC_TRIGGER_T6_TRGO | DAC_CR_DMAEN1 | (DAC_TRIGGER_T6_TRGO << 16);
    DAC1->DHR12RD = AUDIOIF_SILENCE;

    dma_nohal_deinit(&dma_DAC_1_TX);
    dma_nohal_init(&dma_DAC_1_TX, DMA_MEMORY_TO_PERIPH | DMA_CIRCULAR
        | DMA_MDATAALIGN_WORD | DMA_PDATAALIGN_WORD);
    dma_nohal_start(&dma_DAC_1_TX, (uint32_t)audioif_ring, (uint32_t)&DAC1->DHR12RD,
        AUDIOIF_RING_FRAMES);
    DAC1->CR |= DAC_CR_EN1 | DAC_CR_EN2;

    TIM_HandleTypeDef *tim = timer_tim6_init(rate);
    TIM_MasterConfigTypeDef config = {
        .MasterOutputTrigger = TIM_TRGO_UPDATE,
        .MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE,
    };
    HAL_TIMEx_MasterConfigSynchronization(tim, &config);
    HAL_TIM_Base_Start(tim);

    audioif_open = true;
    soft_timer_static_init(&audioif_kick_timer, SOFT_TIMER_MODE_PERIODIC, AUDIOIF_KICK_MS,
        audioif_kick_timer_cb);
    soft_timer_insert(&audioif_kick_timer, AUDIOIF_KICK_MS);
}

// --- what audiobusio.c asks of the output ---------------------------------

bool audiopump_sink_prepare(const char *path, uint32_t rate) {
    (void)path;
    (void)rate;
    return false;
}

bool audiopump_i2s_have(void) {
    return true;
}

bool audiopump_i2s_is_open(void) {
    return audioif_open;
}

// `cfg->dout` is the left channel's pin and `cfg->dac_right` the right's, as
// port * 16 + pin; audioio.AudioOut fills them.
uint32_t audiopump_i2s_open(const audiopump_i2s_cfg_t *cfg) {
    int left = audioif_dac_channel(cfg->dout);
    int right = cfg->dac_right >= 0 ? audioif_dac_channel(cfg->dac_right) : 0;
    if (left == 0 || (cfg->dac_right >= 0 && right == 0) || left == right) {
        mp_raise_ValueError(MP_ERROR_TEXT("the DAC is on PA4 (A12) and PA5 (A13)"));
    }
    if ((uintptr_t)audioif_ring >= 0x20000000u && (uintptr_t)audioif_ring < 0x20020000u) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("this board's .bss is in DTCM, which the DAC's DMA can't read"));
    }
    if (cfg->bits != 16 || cfg->channels < 1 || cfg->channels > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("the DAC plays 16-bit mono or stereo"));
    }
    if (audioif_open) {
        audioif_hw_stop();
    }
    audioif_live = *cfg;
    audioif_channels = (uint8_t)cfg->channels;
    uint32_t ms = cfg->dac_ring_ms > 0 ? (uint32_t)cfg->dac_ring_ms : AUDIOIF_RING_MS;
    uint32_t frames = ((uint32_t)cfg->rate * ms / 1000 + 7) & ~7u;
    audioif_ring_frames = frames < AUDIOIF_RING_MIN ? AUDIOIF_RING_MIN
        : frames > AUDIOIF_RING_MAX ? AUDIOIF_RING_MAX : frames;
    audioif_block = 512 < audioif_ring_frames / 2 ? 512 : audioif_ring_frames / 2;
    audioif_swap = left == 2;
    audioif_hw_start(left, right, (uint32_t)cfg->rate);
    return AUDIOIF_RING_FRAMES * 4;
}

void audiopump_i2s_shutdown(void) {
    if (audioif_open) {
        audioif_hw_stop();
    }
}

bool audiopump_i2s_input_held(void) {
    return false;
}

uint32_t audiopump_i2s_live_rate(void) {
    return audioif_open ? (uint32_t)audioif_live.rate : 0;
}

uint64_t audiopump_i2s_starved(void) {
    return audioif_starved_frames * 2u * audioif_channels;
}

void audiopump_i2s_starved_reset(void) {
    audioif_poll();
    audioif_starved_frames = 0;
    audioif_starved_counting = false;
    audioif_starved_pending = true;
}

void audiopump_i2s_starved_hold(void) {
    audioif_starved_counting = false;
    audioif_starved_pending = false;
}

void audiopump_i2s_starved_resume(void) {
    audioif_poll();
    audioif_starved_pending = true;
}

// The DAC has no capture half; audioi2sin.I2SIn says so.
uint32_t audiopump_i2s_rx_begin(const audiopump_i2s_cfg_t *cfg, uint32_t ring, int take) {
    (void)cfg;
    (void)ring;
    (void)take;
    mp_raise_NotImplementedError(MP_ERROR_TEXT("this board's audio is a DAC, with no I2S input"));
}

uint32_t audiopump_i2s_rx_pull(uint8_t *dst, uint32_t want) {
    (void)dst;
    (void)want;
    return 0;
}

void audiopump_i2s_rx_discard(void) {
}

void audiopump_i2s_rx_counts(uint32_t *frames, uint32_t *dropped) {
    *frames = 0;
    *dropped = 0;
}

void audiopump_i2s_rx_end(void) {
}

bool audiopump_i2s_rx_held(void) {
    return false;
}

// --- the engine's hooks ----------------------------------------------------

static uint64_t audioif_now_us(void) {
    static uint32_t last;
    static uint64_t high;
    uint32_t now = mp_hal_ticks_us();
    if (now < last) {
        high += (uint64_t)1 << 32;
    }
    last = now;
    return high | now;
}

static void audioif_sleep_us(uint64_t us) {
    // Busy, because this must not raise: mp_hal_delay_ms() runs the
    // scheduler, which can.
    while (us > 1000000) {
        mp_hal_delay_us(1000000);
        us -= 1000000;
    }
    mp_hal_delay_us((mp_uint_t)us);
}

static bool audioif_sink_ready(void) {
    return audioif_open;
}

// Signed 16-bit to the DAC's unsigned 12 bits.
static inline uint32_t audioif_u12(int16_t s) {
    return ((uint32_t)((int32_t)s + 32768)) >> 4;
}

static uint32_t audioif_sink_write(const uint8_t *buffer, uint32_t length,
    uint32_t timeout_ms, bool *timed_out) {
    if (!audioif_open) {
        *timed_out = true;
        return 0;
    }
    audioif_poll();
    const uint32_t frame = 2u * audioif_channels;
    uint32_t frames = length / frame;
    if (frames > audioif_block) {
        audioif_block = frames < AUDIOIF_RING_FRAMES / 2 ? frames : AUDIOIF_RING_FRAMES / 2;
    }
    if (audioif_starved_pending) {
        audioif_starved_pending = false;
        audioif_starved_counting = true;
    }
    // The service loop waits for a block's room before it runs the pump, so
    // this is a block bigger than any before it. The DMA frees room as it
    // plays, so wait for it, as long as the engine allows, rather than cut
    // the block short.
    uint32_t space = audioif_space();
    for (uint32_t waited_us = 0; frames > space && waited_us < timeout_ms * 1000u;
         waited_us += 250) {
        mp_hal_delay_us(250);
        audioif_poll();
        space = audioif_space();
    }
    if (frames > space) {
        frames = space;
        *timed_out = true;
    }
    const int16_t *in = (const int16_t *)buffer;
    uint64_t at = audioif_written;
    uint32_t left = frames;
    while (left > 0) {
        uint32_t i = (uint32_t)(at % AUDIOIF_RING_FRAMES);
        uint32_t n = AUDIOIF_RING_FRAMES - i;
        if (n > left) {
            n = left;
        }
        for (uint32_t k = 0; k < n; k++) {
            uint32_t l = audioif_u12(in[0]);
            uint32_t r = audioif_channels == 2 ? audioif_u12(in[1]) : l;
            audioif_ring[i + k] = audioif_swap ? (r | (l << 16)) : (l | (r << 16));
            in += audioif_channels;
        }
        audioif_clean(i, n);
        at += n;
        left -= n;
    }
    mp_uint_t state = MICROPY_BEGIN_ATOMIC_SECTION();
    audioif_written = at;
    MICROPY_END_ATOMIC_SECTION(state);
    return frames * frame;
}

static uint64_t audioif_sink_dma_bytes(void) {
    audioif_poll();
    return audioif_played * 2u * audioif_channels;
}

static void audioif_teardown(void) {
    if (audioif_open) {
        audioif_hw_stop();
    }
}

static const audiodsp_port_ops_t audioif_stm32_ops = {
    .name = "stm32",
    .now_us = audioif_now_us,
    .sleep_us = audioif_sleep_us,
    .sink_ready = audioif_sink_ready,
    .sink_write = audioif_sink_write,
    .sink_dma_bytes = audioif_sink_dma_bytes,
    .teardown = audioif_teardown,
};

const audiodsp_port_ops_t *audiodsp_port_driver(void) {
    return &audioif_stm32_ops;
}

// --- _audioif: what a test or audiodev asks the driver ---------------------

// (written, played, starved frames, service errors, ring frames)
static mp_obj_t audioif_stm32_stats(void) {
    audioif_poll();
    mp_obj_t items[5] = {
        mp_obj_new_int_from_ull(audioif_written),
        mp_obj_new_int_from_ull(audioif_played),
        mp_obj_new_int_from_ull(audioif_starved_frames),
        mp_obj_new_int_from_uint(audioif_service_errors),
        MP_OBJ_NEW_SMALL_INT(AUDIOIF_RING_FRAMES),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(audioif_stm32_stats_obj, audioif_stm32_stats);

static mp_obj_t audioif_stm32_driver(void) {
    return MP_OBJ_NEW_QSTR(qstr_from_str(audioif_stm32_ops.name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(audioif_stm32_driver_obj, audioif_stm32_driver);

static const mp_rom_map_elem_t audioif_stm32_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__audioif) },
    { MP_ROM_QSTR(MP_QSTR_driver), MP_ROM_PTR(&audioif_stm32_driver_obj) },
    { MP_ROM_QSTR(MP_QSTR_dac_stats), MP_ROM_PTR(&audioif_stm32_stats_obj) },
};
static MP_DEFINE_CONST_DICT(audioif_stm32_globals, audioif_stm32_globals_table);

const mp_obj_module_t audioif_stm32_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&audioif_stm32_globals,
};
MP_REGISTER_MODULE(MP_QSTR__audioif, audioif_stm32_module);

#endif // AUDIOIF_DRIVER_STM32
