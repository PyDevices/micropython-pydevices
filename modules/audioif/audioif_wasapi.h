// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2026 PyDevices
//
// WASAPI for the audio pump on Windows: a speaker the pump writes to and a
// microphone the pump reads from, both in shared mode, both on whichever
// thread first touches them -- which is the pump's.
//
// Nothing in here calls into the MicroPython runtime. Every function may run
// on the pump thread, and none of them allocates on the GC heap or raises.

#ifndef AUDIOIF_WASAPI_H
#define AUDIOIF_WASAPI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct audioif_wasapi audioif_wasapi_t;

typedef struct {
    bool capture;           // false: a speaker; true: a microphone
    uint32_t rate;          // frames per second the pump runs at
    uint32_t channels;      // 1 or 2, signed 16-bit
    uint32_t buffer_ms;     // the device buffer asked for (Windows may grant more)
    uint32_t prime_ms;      // speaker: silence queued before the first write
    uint32_t keep_ms;       // microphone: backlog kept beyond one read; 0 keeps all
    char device[128];       // an endpoint ID, or "" for the default device
} audioif_wasapi_cfg_t;

typedef struct {
    uint64_t frames;        // frames handed over (written or read)
    uint64_t starved;       // speaker: writes that found the device queue empty
    uint64_t glitches;      // microphone: packets Windows marked discontinuous
    uint64_t dropped;       // microphone: frames trimmed to hold `keep_ms`
    uint64_t timeouts;      // waits that ran out before the device was ready
    uint32_t buffer_frames; // what Windows granted
    uint32_t queued_frames; // speaker: queued at the last write; mic: waiting
    int32_t error;          // the HRESULT of a failed open, else 0
    uint32_t period_frames; // the engine's default period for this device
    uint32_t latency_frames; // the stream latency Windows reports
} audioif_wasapi_stats_t;

// Allocates the state only. The device opens on the first read or write, on
// that thread, because COM objects live in the apartment that made them.
audioif_wasapi_t *audioif_wasapi_new(const audioif_wasapi_cfg_t *cfg);

// Stop the stream and let go of every COM object. Safe on NULL.
void audioif_wasapi_free(audioif_wasapi_t *w);

// True if the endpoint exists (or "" and there is a default one). Runs on the
// calling thread and leaves nothing open; a bad ID fails here, on the
// interpreter, rather than as silence on the pump.
bool audioif_wasapi_probe(const char *device, bool capture, int32_t *hr);

// The endpoint `device` names (or the default one for the direction): its
// ID, its friendly name and the ID of the physical device it belongs to (its
// container), each as UTF-8, NUL-terminated and cut to fit. Two endpoints in
// one container are one device on one clock, except the container every
// built-in endpoint shares (audioif_wasapi_container_is_host). Runs on the
// calling thread and leaves nothing open.
bool audioif_wasapi_describe(const char *device, bool capture, char *id,
    size_t id_len, char *name, size_t name_len, char *container,
    size_t container_len, int32_t *hr);

// True for the container Windows gives everything built into the computer:
// a laptop's speakers and its microphone array share it whether or not they
// share a clock.
bool audioif_wasapi_container_is_host(const char *container);

// Speaker: queue `length` bytes, waiting up to `timeout_ms` for room. Returns
// the bytes queued.
uint32_t audioif_wasapi_write(audioif_wasapi_t *w, const uint8_t *buffer,
    uint32_t length, uint32_t timeout_ms, bool *timed_out);

// Microphone: fill `length` bytes, waiting up to `timeout_ms`. Returns the
// bytes filled; the caller pads a short read.
uint32_t audioif_wasapi_read(audioif_wasapi_t *w, uint8_t *buffer,
    uint32_t length, uint32_t timeout_ms);

void audioif_wasapi_stats(const audioif_wasapi_t *w,
    audioif_wasapi_stats_t *out);

#endif  // AUDIOIF_WASAPI_H
