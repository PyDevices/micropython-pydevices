// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// The I2S channel, as C rather than as `_audioif.i2s_start(...)`.
//
// `audiobusio.I2SOut` opens the same peripheral the same way -- it is the
// CircuitPython-shaped face of exactly this -- and it lives in its own
// translation unit, so the open has to be reachable from outside
// `_audioif.c`. The Python entry point is now an argument parser in front of
// `audiopump_i2s_open`; there is one implementation and it is this one.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// -1 in any pin or port field means "not wired". `in_port` >= 0 and different
// from `port` is the two-peripheral duplex the T-Embed needs; see the long
// comment at the call site in _audioif.c.
typedef struct {
    int port;
    int bclk;
    int ws;
    int dout;
    int rate;
    int bits;
    int channels;
    int mclk;
    int mclk_fs;
    int dma_desc;
    int dma_frame;
    int din;
    int in_port;
    int in_bclk;
    int in_ws;
    int in_mclk;
} audiopump_i2s_cfg_t;

// Who is holding the open channel. The output (`audiobusio.I2SOut`, or
// `_audioif.i2s_start`) and the capture reader (`_audioif.rx_open`) each hold
// it, and it closes when the last one lets go. That is what lets a board whose
// speaker and microphone share one port record while it plays: both are the
// two halves of ONE channel pair, opened once, on one clock.
#define AUDIOPUMP_I2S_HOLD_OUT (1)
#define AUDIOPUMP_I2S_HOLD_IN  (2)

// One DMA descriptor, in frames, for a given rate: 5 ms, between 64 and 240.
// The output and the capture reader both open the channel, so they must size
// it the same way; the measurement above audiobusio_dma_frame in audiobusio.c
// says why it is sized in time.
static inline int audiopump_i2s_dma_frame(int rate) {
    int frames = rate / 200;
    if (frames < 64) {
        frames = 64;
    }
    if (frames > 240) {
        frames = 240;
    }
    return frames;
}

// True while the capture reader holds the channel. The output must not retune
// it then: the microphone is clocked by the same BCLK, so a new rate would
// change the recording's rate under it.
bool audiopump_i2s_input_held(void);

// The rate the open channel is clocked at, or 0 when nothing is open.
uint32_t audiopump_i2s_live_rate(void);

// True where this build has an I2S peripheral at all -- false on every
// desktop port, which is how audiobusio decides what it is.
bool audiopump_i2s_have(void);

// True while a channel is open.
bool audiopump_i2s_is_open(void);

// Open TX (and RX when `din` >= 0) and enable the channel, for the OUTPUT.
// Raises, so it needs an nlr handler above it. Returns how many bytes the DMA
// holds when full -- the block-to-wire latency floor.
//
// If the capture reader already holds an open channel, the output joins it
// instead, provided it is the same port, pins and rate; anything else raises,
// because the only way to give the output what it asked for would be to
// reopen the channel under a running recording.
uint32_t audiopump_i2s_open(const audiopump_i2s_cfg_t *cfg);

// The output lets go. The channel closes unless the capture reader still
// holds it, in which case TX goes on clocking zeros and RX keeps recording.
// Safe to call when nothing is open.
void audiopump_i2s_shutdown(void);

// Silence on the wire since the performance began, in bytes: descriptors the
// DMA started with nothing written into them, plus any time the DMA itself
// stood still. Both are counted as events in the DMA's interrupt, so the
// number never goes down and reads 0 on a clean playback. 0 on a build with
// no I2S.
uint64_t audiopump_i2s_starved(void);

// A new performance: clear the count, and start counting at the pump's next
// write, so the wait for its first block and the zeros the channel was
// enabled on read as the latency they are. Called by `play()` and when the
// channel opens.
void audiopump_i2s_starved_reset(void);

// A pause begins: the zeros it clocks were asked for, so stop counting and
// keep the total.
void audiopump_i2s_starved_hold(void);

// The pause ends: count again from the pump's next write.
void audiopump_i2s_starved_resume(void);

#ifdef __cplusplus
}
#endif
