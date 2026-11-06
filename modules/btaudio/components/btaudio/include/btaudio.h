// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio: receive Bluetooth audio (A2DP sink) and talk to the sending
// phone or computer (AVRCP). Plain ESP-IDF, no MicroPython: the binding in
// ../../src wraps it, and the same component builds on any chip with
// Bluetooth Classic and either IDF 5.5 or 6.x.
//
// Audio arrives decoded, as 16-bit stereo PCM, into a ring buffer;
// btaudio_read() takes it out at the rate of whatever plays it. The two
// clocks never agree exactly, so btaudio_read() holds the ring near half
// full by dropping or repeating one frame now and then.
//
// Every call is safe from any task except btaudio_start() and
// btaudio_stop(), which one task makes.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bits returned by btaudio_changes(): what changed since the last call.
#define BTAUDIO_CHANGED_CONNECTION (1u << 0)
#define BTAUDIO_CHANGED_STREAM     (1u << 1)
#define BTAUDIO_CHANGED_FORMAT     (1u << 2)
#define BTAUDIO_CHANGED_TRACK      (1u << 3)
#define BTAUDIO_CHANGED_PLAYING    (1u << 4)
#define BTAUDIO_CHANGED_VOLUME     (1u << 5)

typedef enum {
    BTAUDIO_TEXT_TITLE = 0,
    BTAUDIO_TEXT_ARTIST,
    BTAUDIO_TEXT_ALBUM,
    BTAUDIO_TEXT_PEER,      // the sender's Bluetooth address, as text
    BTAUDIO_TEXT_COUNT,
} btaudio_text_t;

typedef enum {
    BTAUDIO_CMD_PLAY = 0,
    BTAUDIO_CMD_PAUSE,
    BTAUDIO_CMD_NEXT,
    BTAUDIO_CMD_PREVIOUS,
    BTAUDIO_CMD_STOP,
} btaudio_cmd_t;

typedef struct {
    bool connected;         // an A2DP link is up
    bool streaming;         // audio packets are flowing
    bool playing;           // the sender says it is playing (AVRCP)
    uint32_t sample_rate;   // of the decoded stream, 0 before the first one
    uint8_t volume;         // 0..127, AVRCP's absolute volume
    uint32_t buffered;      // frames in the ring now
    uint32_t capacity;      // frames the ring holds
    uint32_t underruns;     // reads that ran the ring dry
    uint32_t overruns;      // packets that found the ring full
    uint32_t dropped;       // frames dropped to keep up with the sender
    uint32_t repeated;      // frames repeated to wait for it
    uint32_t received;      // frames decoded since start
} btaudio_status_t;

// Start Bluetooth and become discoverable as `name`. `ring_frames` sizes
// the buffer in stereo frames (0 picks about half a second at 44.1 kHz).
// Releases the controller's BLE memory, so BLE can't start later in this
// boot. ESP_ERR_INVALID_STATE if already started.
esp_err_t btaudio_start(const char *name, size_t ring_frames);

// Disconnect and shut Bluetooth down. btaudio_start() may run again.
void btaudio_stop(void);

bool btaudio_started(void);

// Fill `dst` with `frames` stereo frames (16-bit, interleaved L R), at the
// stream's rate, scaled by the volume. Silence while nothing is buffered.
// Always fills every frame; returns how many came from the stream.
size_t btaudio_read(int16_t *dst, size_t frames);

// The bits above for what changed since the last call, then clears them.
uint32_t btaudio_changes(void);

void btaudio_status(btaudio_status_t *out);

// Copy one text field, NUL-terminated, truncated to `size`. Returns its
// length as copied.
size_t btaudio_text(btaudio_text_t which, char *dst, size_t size);

// Peak sample of each channel since the last call (0..32767), before the
// volume is applied, then resets both.
void btaudio_levels(uint16_t *left, uint16_t *right);

// Send a transport command to the sender.
esp_err_t btaudio_command(btaudio_cmd_t cmd);

// Set the volume (0..127) here and tell the sender, which moves its slider.
void btaudio_set_volume(uint8_t volume);

// Disconnect the current sender; discoverable again afterwards.
esp_err_t btaudio_disconnect(void);

#ifdef __cplusplus
}
#endif
