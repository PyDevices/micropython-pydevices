// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btclassic: Bluetooth Classic for a board, as a backend any Bluetooth host
// can implement. The radio (GAP: name, visibility, pairing), the audio sink
// (A2DP sink with the AVRCP controller, cover art included) and the
// hands-free unit (HFP HF, with call audio both ways).
//
// This header has no MicroPython and no ESP-IDF in it. The MicroPython
// binding in ../../../src talks to nothing else, so another host stack can
// stand behind it. The one here is ESP-IDF's Bluedroid (radio.c,
// a2dp_sink.c, avrcp_ct.c, hfp_hf.c); ring.c, the audio buffer, is plain C
// that any backend can use.
//
// Threads: the stack calls in from its own task. Every function here is
// safe from any other task, except the start and stop functions, which one
// task makes. Nothing here calls out: callers poll the *_changes() bits.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- results ----

#define BTC_OK              0
#define BTC_ERR_STATE       (-1)    // not on, not started, or not connected
#define BTC_ERR_NOMEM       (-2)
#define BTC_ERR_UNSUPPORTED (-3)    // this build or backend has no such thing
#define BTC_ERR_ARG         (-4)
#define BTC_ERR_FAIL        (-5)    // the stack refused

// A value not known yet: the other side hasn't said, or doesn't say.
#define BTC_UNKNOWN (-1)

// The bits the *_changes() functions return: what changed since the last
// call. The Python constants of the same names have these values.
#define BTC_CHANGED_CONNECTION (1u << 0)    // sink, hf: the profile's link
#define BTC_CHANGED_STREAM     (1u << 1)    // sink: audio started or stopped
#define BTC_CHANGED_FORMAT     (1u << 2)    // sink: the stream's rate
#define BTC_CHANGED_TRACK      (1u << 3)    // sink: any track metadata
#define BTC_CHANGED_PLAYING    (1u << 4)    // sink: play status
#define BTC_CHANGED_VOLUME     (1u << 5)    // sink
#define BTC_CHANGED_POSITION   (1u << 6)    // sink: the sender reported a position
#define BTC_CHANGED_PEER       (1u << 7)    // sink: name, signal, battery, link details
#define BTC_CHANGED_COVER      (1u << 8)    // sink: a new cover image, or none
#define BTC_CHANGED_PHONE      (1u << 9)    // hf: battery, signal, carrier, gains ...
#define BTC_CHANGED_CALL       (1u << 10)   // hf: call state or caller
#define BTC_CHANGED_AUDIO      (1u << 11)   // hf: call audio came or went
#define BTC_CHANGED_PAIRED     (1u << 12)   // radio: a device paired or was unpaired
#define BTC_CHANGED_REMOTE     (1u << 13)   // source: the remote pressed or released a key
#define BTC_CHANGED_FOUND      (1u << 14)   // radio: an inquiry found a device, or ended

typedef uint8_t btc_addr_t[6];  // most significant byte first

// The audio buffer's counters, in frames.
typedef struct {
    uint32_t buffered;      // in the ring now
    uint32_t capacity;      // the ring holds
    uint32_t received;      // since start
    uint32_t underruns;     // reads that ran the ring dry
    uint32_t overruns;      // writes that found the ring full
    uint32_t dropped;       // frames dropped to keep up with the sender
    uint32_t repeated;      // frames repeated to wait for it
    uint32_t skipped;       // frames skipped to start at the fill point
} btc_ring_stats_t;

// ---- the radio ----

// Start the controller and the host, connectable and discoverable as the
// settings say. Settings made before start apply at start.
int btc_radio_start(void);
// End every profile and shut Bluetooth down. btc_radio_start() may run again.
void btc_radio_stop(void);
bool btc_radio_active(void);

int btc_radio_set_name(const char *name);       // up to 248 bytes of UTF-8
size_t btc_radio_name(char *dst, size_t size);
int btc_radio_set_visibility(bool discoverable, bool connectable);
void btc_radio_visibility(bool *discoverable, bool *connectable);
int btc_radio_set_pin(const char *pin);         // 1 to 16 characters
size_t btc_radio_pin(char *dst, size_t size);
int btc_radio_mac(btc_addr_t mac);              // BTC_ERR_STATE while off

// The paired devices: copies up to `max` addresses and returns how many
// there are (which may be more than `max`), or a negative BTC_ERR_*.
int btc_radio_paired(btc_addr_t *addrs, int max);
// Forget one paired device, or all of them with NULL.
int btc_radio_unpair(const uint8_t *addr);
uint32_t btc_radio_changes(void);

// ---- the audio sink: A2DP sink and AVRCP controller ----

typedef enum {
    BTC_SINK_TITLE = 0,
    BTC_SINK_ARTIST,
    BTC_SINK_ALBUM,
    BTC_SINK_GENRE,
    BTC_SINK_PEER_NAME,     // the sender's device name
    BTC_SINK_TEXT_COUNT,
} btc_sink_text_t;

// AVRCP play status, as the sender reports it.
typedef enum {
    BTC_PLAY_STOPPED = 0,
    BTC_PLAY_PLAYING = 1,
    BTC_PLAY_PAUSED = 2,
    BTC_PLAY_FWD_SEEK = 3,
    BTC_PLAY_REV_SEEK = 4,
    BTC_PLAY_ERROR = 0xff,
} btc_play_status_t;

typedef struct {
    bool connected;         // an A2DP link is up
    bool streaming;         // audio packets are flowing
    btc_addr_t peer;        // the sender, while connected
    uint32_t sample_rate;   // of the decoded stream, 0 before the first one
    uint8_t volume;         // 0..127, AVRCP's absolute volume

    // AVRCP: the track and the player. BTC_UNKNOWN where not known.
    int play_status;        // btc_play_status_t
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
} btc_sink_status_t;

// Start the sink (the radio must be on). `ring_frames` sizes the buffer in
// stereo frames. With `reconnect`, call back the last sender if it is still
// paired. BTC_ERR_STATE if already started or the radio is off.
int btc_sink_start(size_t ring_frames, bool reconnect);
void btc_sink_stop(void);
bool btc_sink_started(void);

// Fill `dst` with `frames` stereo frames (16-bit, interleaved L R) at the
// stream's rate, scaled by the volume. Silence while nothing is buffered.
// Always fills every frame; returns how many came from the stream.
size_t btc_sink_read(int16_t *dst, size_t frames);
uint32_t btc_sink_changes(void);
void btc_sink_status(btc_sink_status_t *out);
void btc_sink_stats(btc_ring_stats_t *out);
// Copy one text field, NUL-terminated, truncated to `size`; returns its length.
size_t btc_sink_text(btc_sink_text_t which, char *dst, size_t size);
// The current cover image, a JPEG (AVRCP's 200x200 linked thumbnail).
// btc_sink_cover_size() is 0 with none. btc_sink_cover() copies up to
// `size` bytes and returns how many, or 0 if the cover changed meanwhile.
size_t btc_sink_cover_size(void);
size_t btc_sink_cover(uint8_t *dst, size_t size);
// Peak sample of each channel since the last call (0..32767), before the
// volume is applied.
void btc_sink_levels(uint16_t *left, uint16_t *right);

// Send an AVRCP pass-through key (operation id 0..0x7f): `state` 0 presses
// it, 1 releases it, 2 does both. Play is 0x44, stop 0x45, pause 0x46,
// rewind 0x48, fast forward 0x49, next 0x4b, previous 0x4c.
int btc_sink_key(uint8_t key, int state);
// Set one of the sender's player settings (AVRCP attribute 1..4 and value).
int btc_sink_set_player(uint8_t attr, uint8_t value);
// Ask the sender for its play status, position and signal now.
int btc_sink_refresh(void);
// Set the volume (0..127) here and tell the sender.
void btc_sink_set_volume(uint8_t volume);
// Call a paired sender, or the last one with NULL.
int btc_sink_connect(const uint8_t *addr);
int btc_sink_disconnect(void);

// ---- the hands-free unit: HFP HF ----

typedef enum {
    BTC_HF_OPERATOR = 0,    // the phone's network operator
    BTC_HF_CALLER,          // the number of the current or last call
    BTC_HF_SUBSCRIBER,      // the phone's own number, if it says
    BTC_HF_TEXT_COUNT,
} btc_hf_text_t;

// The call's state, from the phone's indicators.
typedef enum {
    BTC_CALL_IDLE = 0,
    BTC_CALL_INCOMING,      // ringing
    BTC_CALL_DIALING,
    BTC_CALL_ALERTING,      // the other end is ringing
    BTC_CALL_ACTIVE,
    BTC_CALL_HELD,
} btc_call_t;

typedef struct {
    bool connected;         // the service level connection is up
    btc_addr_t peer;        // the phone, while connected
    int service;            // network service: 0 none, 1 available; BTC_UNKNOWN
    int signal;             // 0..5
    int roaming;            // 0 or 1
    int battery;            // 0..5
    int call;               // btc_call_t
    int held;               // 0 none, 1 held and active, 2 held only
    bool waiting;           // a second call is waiting
    bool ringing;           // the phone sent RING
    bool voice_recognition; // the phone's assistant is listening
    bool in_band_ring;      // the phone sends its ringtone as call audio
    int speaker_gain;       // 0..15; the call plays at this
    int mic_gain;           // 0..15
    bool audio;             // the call's audio link is up
    uint32_t sample_rate;   // 8000 or 16000 while it is, else 0
} btc_hf_status_t;

// Start the hands-free unit (the radio must be on). `ring_ms` sizes the
// buffer for the phone's voice; `channels` (1 or 2) is the frame shape
// btc_hf_read() fills. While a sink is started, the unit follows its
// sender: when one connects, so does the hands-free link.
int btc_hf_start(uint32_t ring_ms, uint8_t channels);
void btc_hf_stop(void);
bool btc_hf_started(void);

// Fill `dst` with `frames` frames of the phone's voice, at the call's rate,
// scaled by the speaker gain. Silence while nothing is buffered; returns
// how many came from the call.
size_t btc_hf_read(int16_t *dst, size_t frames);
// Queue microphone audio for the phone: 16-bit mono at the call's rate.
// Returns the frames taken, fewer when the queue (half a second) is full, 0
// without call audio. It goes out once 120 ms is queued; silence meanwhile.
size_t btc_hf_write(const int16_t *pcm, size_t frames);
uint32_t btc_hf_changes(void);
void btc_hf_status(btc_hf_status_t *out);
void btc_hf_stats(btc_ring_stats_t *out);
size_t btc_hf_text(btc_hf_text_t which, char *dst, size_t size);

// Calls. BTC_ERR_STATE without a link.
int btc_hf_answer(void);
int btc_hf_hangup(void);
int btc_hf_dial(const char *number);        // NULL redials the last number
int btc_hf_dtmf(char digit);
int btc_hf_voice_recognition(bool on);
int btc_hf_set_speaker_gain(int gain);      // 0..15, told to the phone
int btc_hf_set_mic_gain(int gain);
int btc_hf_connect_audio(void);
int btc_hf_disconnect_audio(void);
// Call a paired phone, or the sink's sender with NULL.
int btc_hf_connect(const uint8_t *addr);
int btc_hf_disconnect(void);

// ---- discovery: inquiry, on the radio ----

// A device an inquiry found. Fields it didn't send are empty, 0 or BTC_UNKNOWN.
typedef struct {
    btc_addr_t addr;
    char name[64];          // from its inquiry response, UTF-8; "" if it sent none
    uint32_t cod;           // Class of Device: service bits, major and minor class
    int rssi;               // dBm, or BTC_UNKNOWN
} btc_found_t;

#define BTC_FOUND_MAX 20    // devices one inquiry keeps

// Look for discoverable devices for `duration_ms` (1280 to 61440, in steps
// of 1.28 s), forgetting the last inquiry's results. 0 stops one running.
int btc_radio_inquiry(uint32_t duration_ms);
bool btc_radio_inquiring(void);
// Copy up to `max` of the devices found so far; returns how many there are.
int btc_radio_found(btc_found_t *out, int max);

// ---- the audio source: A2DP source and AVRCP target ----
//
// The board plays to headphones, a speaker or another board. Bluedroid runs
// one A2DP role at a time: btc_source_start() fails with BTC_ERR_STATE
// while the sink is started, and btc_sink_start() while the source is.
//
// Audio goes in with btc_source_write() from any one task: a program's
// write(), or a C producer such as an audio graph's output. It is resampled
// to the stream's rate and widened to its channels, and buffered in a ring
// Bluedroid's encoder task drains at the stream's clock.

typedef enum {
    BTC_SOURCE_PEER_NAME = 0,   // the sink's device name
    BTC_SOURCE_TITLE,           // what the AVRCP target tells the remote
    BTC_SOURCE_ARTIST,
    BTC_SOURCE_ALBUM,
    BTC_SOURCE_GENRE,
    BTC_SOURCE_TEXT_COUNT,
} btc_source_text_t;

typedef struct {
    bool connected;         // an A2DP link to a sink is up
    bool streaming;         // the stream is started and audio is flowing
    btc_addr_t peer;
    uint32_t sample_rate;   // the stream's rate (44100 on IDF 5.x), 0 before one
    uint8_t channels;       // the stream's channels, 1 or 2
    uint8_t volume;         // 0..127
    bool absolute_volume;   // the sink takes the volume; else it scales the samples
    bool avrcp;             // the remote control link is up
    uint8_t sbc_channel_mode, sbc_block_length, sbc_subbands, sbc_allocation;
    uint8_t sbc_min_bitpool, sbc_max_bitpool;
    uint16_t delay_ms;      // the delay the sink reported, 0 if none
    bool metadata;          // this build answers the remote's track and play status queries
    int play_status;        // what the target reports: btc_play_status_t
    int64_t position_ms;    // and where, counted on while playing; BTC_UNKNOWN
} btc_source_status_t;

// Start the source (the radio must be on). `ring_frames` sizes the buffer
// in stereo frames at the stream's rate.
int btc_source_start(size_t ring_frames);
void btc_source_stop(void);
bool btc_source_started(void);

// Call a sink, or the last one with NULL. The stream starts once it's up.
int btc_source_connect(const uint8_t *addr);
int btc_source_disconnect(void);
// Start or suspend the stream on a connected sink.
int btc_source_stream(bool on);

// Queue `frames` frames of 16-bit PCM, `channels` (1 or 2) interleaved, at
// `rate` Hz (8000..48000). Returns the frames taken: fewer when the buffer
// holds its share (half, plus a little), so a producer that writes as fast
// as it can is paced by the stream. Never waits.
size_t btc_source_write(const int16_t *pcm, size_t frames, uint32_t rate, uint8_t channels);
uint32_t btc_source_changes(void);
void btc_source_status(btc_source_status_t *out);
void btc_source_stats(btc_ring_stats_t *out);
size_t btc_source_text(btc_source_text_t which, char *dst, size_t size);
// Set the volume (0..127): sent to a sink that takes absolute volume,
// otherwise applied to the samples.
void btc_source_set_volume(uint8_t volume);

// The remote's keys, oldest first: AVRCP operation id (0..0x7f) in the low
// byte, bit 8 set for a press and clear for a release. -1 with none queued.
int btc_source_key(void);

// What the AVRCP target tells the remote (a car stereo shows it). NULL
// texts leave a field as it is; BTC_UNKNOWN numbers clear one. A new title
// counts as a new track.
int btc_source_set_track(const char *title, const char *artist, const char *album,
    const char *genre, int track_number, int track_count, int64_t duration_ms);
// The play status (btc_play_status_t), or BTC_UNKNOWN to keep it, and the
// position in milliseconds, or BTC_UNKNOWN to keep counting from the last
// one while playing.
int btc_source_set_play_status(int status, int64_t position_ms);

#ifdef __cplusplus
}
#endif
