// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio: receive Bluetooth audio (A2DP sink), talk to the sending phone or
// computer about what it plays (AVRCP, cover art included), and follow the
// phone itself (HFP hands-free: battery, signal, carrier and calls). Plain
// ESP-IDF, no MicroPython: the binding in ../../src wraps it, and the same
// component builds on any chip with Bluetooth Classic and either IDF 5.5 or
// 6.x. Cover art and HFP build only when the project's sdkconfig turns them
// on (BT_AVRCP_CT_COVER_ART_ENABLED, BT_HFP_CLIENT_ENABLE).
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
#define BTAUDIO_CHANGED_TRACK      (1u << 3)    // any track metadata
#define BTAUDIO_CHANGED_PLAYING    (1u << 4)    // play status
#define BTAUDIO_CHANGED_VOLUME     (1u << 5)
#define BTAUDIO_CHANGED_POSITION   (1u << 6)    // the sender reported a position
#define BTAUDIO_CHANGED_PEER       (1u << 7)    // name, signal, battery, link details
#define BTAUDIO_CHANGED_COVER      (1u << 8)    // a new cover image, or none
#define BTAUDIO_CHANGED_PHONE      (1u << 9)    // HFP: battery, signal, carrier ...
#define BTAUDIO_CHANGED_CALL       (1u << 10)   // HFP: call state or caller

// Not known yet: the sender hasn't said, or doesn't say.
#define BTAUDIO_UNKNOWN (-1)

typedef enum {
    BTAUDIO_TEXT_TITLE = 0,
    BTAUDIO_TEXT_ARTIST,
    BTAUDIO_TEXT_ALBUM,
    BTAUDIO_TEXT_GENRE,
    BTAUDIO_TEXT_PEER,          // the sender's Bluetooth address, as text
    BTAUDIO_TEXT_PEER_NAME,     // the sender's device name
    BTAUDIO_TEXT_OPERATOR,      // HFP: the phone's network operator
    BTAUDIO_TEXT_CALLER,        // HFP: the number of the current or last call
    BTAUDIO_TEXT_SUBSCRIBER,    // HFP: the phone's own number, if it says
    BTAUDIO_TEXT_COUNT,
} btaudio_text_t;

typedef enum {
    BTAUDIO_CMD_PLAY = 0,
    BTAUDIO_CMD_PAUSE,
    BTAUDIO_CMD_NEXT,
    BTAUDIO_CMD_PREVIOUS,
    BTAUDIO_CMD_STOP,
} btaudio_cmd_t;

// AVRCP play status, as the sender reports it.
typedef enum {
    BTAUDIO_PLAY_STOPPED = 0,
    BTAUDIO_PLAY_PLAYING = 1,
    BTAUDIO_PLAY_PAUSED = 2,
    BTAUDIO_PLAY_FWD_SEEK = 3,
    BTAUDIO_PLAY_REV_SEEK = 4,
    BTAUDIO_PLAY_ERROR = 0xff,
} btaudio_play_status_t;

// HFP call state, from the phone's indicators and call list.
typedef enum {
    BTAUDIO_CALL_IDLE = 0,
    BTAUDIO_CALL_INCOMING,      // ringing
    BTAUDIO_CALL_DIALING,
    BTAUDIO_CALL_ALERTING,      // the other end is ringing
    BTAUDIO_CALL_ACTIVE,
    BTAUDIO_CALL_HELD,
} btaudio_call_t;

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

    // AVRCP: the track and the player. BTAUDIO_UNKNOWN where not known.
    int play_status;        // btaudio_play_status_t
    int track_number;       // position on the album, from 1
    int track_count;        // tracks on the album
    int64_t duration_ms;    // the track's length
    int64_t position_ms;    // where playback is now (moves on while playing)
    int battery;            // the sender's AVRCP battery state, 0..4
    uint32_t avrcp_features;    // the sender's AVRCP feature mask
    uint16_t avrcp_tg_flags;    // and its target feature flags
    bool cover_art;         // the cover art channel is open

    // The link.
    int rssi_delta;         // dB from the controller's golden range; 0 inside it
    uint8_t channels;       // of the stream as sent
    uint8_t sbc_channel_mode;   // the A2DP SBC configuration bits, as sent
    uint8_t sbc_block_length;
    uint8_t sbc_subbands;
    uint8_t sbc_allocation;
    uint8_t sbc_min_bitpool;
    uint8_t sbc_max_bitpool;
    uint16_t delay_ms;      // the delay reported to the sender

    // HFP: the phone.
    bool hf_connected;      // the hands-free link is up
    int service;            // network service: 0 none, 1 available
    int signal;             // 0..5
    int roaming;            // 0 or 1
    int phone_battery;      // 0..5
    int call;               // btaudio_call_t
    int held;               // calls held: 0 none, 1 held and active, 2 held only
    bool waiting;           // a second call is waiting
    bool ringing;           // the phone sent RING
    bool voice;             // voice recognition is on
    bool in_band_ring;      // the phone sends its ringtone over the link
    int speaker_gain;       // 0..15, as the phone sets it
    int mic_gain;           // 0..15
} btaudio_status_t;

// Start Bluetooth and become discoverable as `name`. `ring_frames` sizes
// the buffer in stereo frames (0 picks about half a second at 44.1 kHz).
// With `reconnect`, call back the last sender if it is still paired, as a
// speaker does when it's switched on; the address is kept in NVS.
// Releases the controller's BLE memory, so BLE can't start later in this
// boot. ESP_ERR_INVALID_STATE if already started.
esp_err_t btaudio_start(const char *name, size_t ring_frames, bool reconnect);

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

// The current cover image, a JPEG (AVRCP's 200x200 linked thumbnail).
// btaudio_cover_size() is 0 with none. btaudio_cover() copies up to `size`
// bytes and returns how many, or 0 if the cover changed size meanwhile.
size_t btaudio_cover_size(void);
size_t btaudio_cover(uint8_t *dst, size_t size);

// Peak sample of each channel since the last call (0..32767), before the
// volume is applied, then resets both.
void btaudio_levels(uint16_t *left, uint16_t *right);

// Send a transport command to the sender: a press and a release.
esp_err_t btaudio_command(btaudio_cmd_t cmd);

// Send any AVRCP pass-through key (ESP_AVRC_PT_CMD_*): `state` 0 presses
// it, 1 releases it, 2 does both. A held key repeats on the sender, which
// is how fast forward and rewind run until released.
esp_err_t btaudio_key(uint8_t key, int state);

// Set one of the sender's player settings (ESP_AVRC_PS_* attribute and
// value: equalizer, repeat, shuffle, scan). The sender may ignore it.
esp_err_t btaudio_set_player(uint8_t attr, uint8_t value);

// Ask the sender for its play status, position and signal now, rather than
// at the next one-second poll.
esp_err_t btaudio_refresh(void);

// Set the volume (0..127) here and tell the sender, which moves its slider.
void btaudio_set_volume(uint8_t volume);

// Disconnect the current sender; discoverable again afterwards.
esp_err_t btaudio_disconnect(void);

// HFP: answer or end/reject a call, dial (NULL redials the last number),
// voice recognition on or off, and a DTMF digit. ESP_ERR_INVALID_STATE
// without a hands-free link, ESP_ERR_NOT_SUPPORTED in a build without HFP.
// Call audio isn't carried: a call the phone hands to the board is handed
// straight back, so it stays on the phone.
esp_err_t btaudio_answer(void);
esp_err_t btaudio_hangup(void);
esp_err_t btaudio_dial(const char *number);
esp_err_t btaudio_voice(bool on);
esp_err_t btaudio_dtmf(char digit);

#ifdef __cplusplus
}
#endif
