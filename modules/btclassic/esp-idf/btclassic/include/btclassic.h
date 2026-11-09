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
#define BTC_CHANGED_SERIAL     (1u << 15)   // spp: bytes arrived
#define BTC_CHANGED_HID        (1u << 16)   // hid: a report came, the host set one, or room to send
#define BTC_CHANGED_PHONEBOOK  (1u << 17)   // pb: a request finished, or the link came or went
#define BTC_CHANGED_REQUEST    (1u << 18)   // ag: the headset dialed, sent a DTMF digit or a command
#define BTC_CHANGED_MESSAGES   (1u << 19)   // msg: a request finished, or the link came or went
#define BTC_CHANGED_NOTIFY     (1u << 20)   // msg: the phone reported a new, sent, deleted or changed message
#define BTC_CHANGED_TETHER     (1u << 21)   // tether: the link came or went

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
// BLE's share of the stack, for MicroPython's `bluetooth` module: the
// controller and host come up with whichever of Classic and BLE starts
// first, and go down with the last to stop.
int btc_stack_ble_start(void);
void btc_stack_ble_stop(void);
bool btc_radio_active(void);

int btc_radio_set_name(const char *name);       // up to 248 bytes of UTF-8
size_t btc_radio_name(char *dst, size_t size);
int btc_radio_set_visibility(bool discoverable, bool connectable);
void btc_radio_visibility(bool *discoverable, bool *connectable);
int btc_radio_set_pin(const char *pin);         // 1 to 16 characters
size_t btc_radio_pin(char *dst, size_t size);
int btc_radio_mac(btc_addr_t mac);              // BTC_ERR_STATE while off
// The Class of Device, as 24 bits. 0 (the default) lets the profiles running
// decide: Audio/Video with any audio profile, Peripheral for a HID device
// alone, with a service bit for each audio profile. Anything else is used
// as it is, whatever runs.
int btc_radio_set_class(uint32_t cod);
uint32_t btc_radio_class(void);                 // the class in use

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
// paired, at start and whenever its link drops (not by
// btc_sink_disconnect()), every few seconds for about a minute, or until
// a call turns out the sender has forgotten the board.
// BTC_ERR_STATE if already started or the radio is off.
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
// Bluedroid's audio gateway has global functions of these four names, so
// the hands-free unit's link under other symbols. Code keeps these names.
#define btc_hf_connect_audio btclassic_hf_connect_audio
#define btc_hf_disconnect_audio btclassic_hf_disconnect_audio
#define btc_hf_connect btclassic_hf_connect
#define btc_hf_disconnect btclassic_hf_disconnect
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

// ---- serial port (SPP) ----------------------------------------------------
//
// A serial port over RFCOMM, one link at a time. Starting it serves a port
// others can connect to (a PC's Bluetooth COM port, a phone's terminal app);
// btc_spp_connect() calls another device's. Bytes arrive in a receive ring
// of `rx_bytes`, more than which are dropped and counted, and leave through
// a send ring. Changes: BTC_CHANGED_CONNECTION, BTC_CHANGED_SERIAL.

typedef struct {
    bool connected;
    bool listening;         // the server is up for others to connect to
    btc_addr_t peer;
    uint8_t channel;        // the server's RFCOMM channel, 0 before it's up
    uint16_t mtu;           // 0 when the stack doesn't say
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_dropped;    // bytes that found the receive ring full
} btc_spp_status_t;

int btc_spp_start(const char *service_name, size_t rx_bytes);
void btc_spp_stop(void);
bool btc_spp_started(void);
bool btc_spp_connected(void);
// channel 0 finds the device's serial port by SDP.
int btc_spp_connect(const uint8_t *addr, int channel);
int btc_spp_disconnect(void);
size_t btc_spp_any(void);                       // bytes waiting to be read
size_t btc_spp_read(uint8_t *dst, size_t size); // never waits
size_t btc_spp_write(const uint8_t *src, size_t size);  // what fits; never waits
size_t btc_spp_write_room(void);
uint32_t btc_spp_changes(void);
void btc_spp_status(btc_spp_status_t *out);

// ---- HID: the board as a keyboard, mouse or gamepad, or the host of one ---
//
// One role at a time: both take the same two L2CAP channels. Reports go in
// and out as the descriptor lays them out, report ID first when the
// descriptor has IDs; the backend adds and strips the HID transaction header.
// Changes: BTC_CHANGED_CONNECTION, BTC_CHANGED_HID.

typedef struct {
    bool started;
    bool host;              // the role
    bool connected;
    btc_addr_t peer;
    bool boot;              // the link runs the boot protocol
    uint32_t sent, received, dropped;
    uint32_t queued;        // bytes waiting in the report queue
} btc_hid_status_t;

// `subclass` is the minor device class: 0x40 keyboard, 0x80 mouse, 0xc0
// both, 0x08 gamepad. `boot`: the descriptor's first reports follow the
// boot protocol's keyboard (ID 1) or mouse (ID 2) layout.
int btc_hid_device_start(const uint8_t *descriptor, size_t len, uint8_t subclass, const char *name, bool boot);
int btc_hid_device_connect(const uint8_t *addr);      // NULL: the last host
// Queue one input report; 1 if queued, 0 if the queue is full or no host
// is connected (a report kept for a later host would reach whatever has
// its focus by then).
int btc_hid_device_send(const uint8_t *report, size_t len);
// The last output report the host set (a keyboard's LEDs), ID first.
size_t btc_hid_device_output(uint8_t *dst, size_t size);
// A feature report the host can read with GET_REPORT, ID first when the
// descriptor has IDs; the host may set it too. Up to 4 IDs.
int btc_hid_device_set_feature(const uint8_t *report, size_t len);
// A feature report as it stands now, ID first; 0 for none.
size_t btc_hid_device_feature(uint8_t report_id, uint8_t *dst, size_t size);
// The Device ID record a host reads to know the product: a USB vendor ID,
// product ID and version, put in as the device starts. All 0 for none.
int btc_hid_device_set_ids(uint16_t vendor, uint16_t product, uint16_t version);
int btc_hid_host_start(void);
int btc_hid_host_connect(const uint8_t *addr, bool boot);
// The next input report, oldest first; 0 with none.
size_t btc_hid_host_read(uint8_t *dst, size_t size);
// Send an output report: `report_id` 0 when the descriptor has none.
int btc_hid_host_send(uint8_t report_id, const uint8_t *data, size_t len);
// The connected device's report descriptor; 0 before it is known.
size_t btc_hid_host_descriptor(uint8_t *dst, size_t size);
// The connected device's vendor and product IDs and version, from its
// service record; false before they are known.
bool btc_hid_host_ids(uint16_t *vendor, uint16_t *product, uint16_t *version);
// Report types, as HID numbers them.
#define BTC_HID_INPUT 1
#define BTC_HID_OUTPUT 2
#define BTC_HID_FEATURE 3
// Ask the connected device for one report over the control channel (a
// gamepad's settings, say); `report_id` 0 when the descriptor has none.
// One request at a time: btc_hid_host_got_report() gives the answer.
int btc_hid_host_get_report(uint8_t type, uint8_t report_id);
// The answer, report ID first when there is one: its length, 0 while it
// hasn't come, or a negative BTC_ERR_* (BTC_ERR_FAIL: the device refused).
int btc_hid_host_got_report(uint8_t *dst, size_t size);
// Set one report over the control channel, report ID first when the
// descriptor has IDs. Returns once the request is sent; it shares the one
// request at a time with btc_hid_host_get_report().
int btc_hid_host_set_report(uint8_t type, const uint8_t *report, size_t len);
// The device's answer to the set: 1 accepted, 0 while it hasn't come, or a
// negative BTC_ERR_* (BTC_ERR_FAIL: the device refused).
int btc_hid_host_set_done(void);
int btc_hid_disconnect(void);
void btc_hid_stop(void);
void btc_hid_status(btc_hid_status_t *out);
uint32_t btc_hid_changes(void);

// ---- the phone book: PBAP client -------------------------------------------
//
// Reads a phone's contacts and call history. One request runs at a time: it
// returns at once, and BTC_CHANGED_PHONEBOOK (with `busy` false) says it
// finished; btc_pb_data() then holds the reply. Paths are PBAP's: the books
// are "telecom/pb" (contacts), "telecom/ich", "och", "mch" (incoming,
// outgoing, missed calls), "telecom/cch" (all calls), "telecom/spd" and
// "telecom/fav", and the same under "SIM1/" for the SIM card's.
// Changes: BTC_CHANGED_CONNECTION, BTC_CHANGED_PHONEBOOK.

// PBAP's vCard property bits, for the `properties` filters (0 means all).
#define BTC_PB_PROP_VERSION   (1ull << 0)
#define BTC_PB_PROP_FN        (1ull << 1)     // the formatted name
#define BTC_PB_PROP_N         (1ull << 2)     // the structured name
#define BTC_PB_PROP_PHOTO     (1ull << 3)
#define BTC_PB_PROP_TEL       (1ull << 7)
#define BTC_PB_PROP_EMAIL     (1ull << 8)
#define BTC_PB_PROP_ORG       (1ull << 16)
#define BTC_PB_PROP_CALL_TIME (1ull << 28)    // X-IRMC-CALL-DATETIME, in call history

#define BTC_PB_ALL 0xffff   // a count meaning "every entry"

typedef struct {
    bool started;
    bool connected;
    btc_addr_t peer;
    uint8_t repositories;   // the phone's: 1 phone, 2 SIM, 4 speed dial, 8 favourites
    uint32_t features;      // the phone's PBAP features
    bool busy;              // a request is running
    int result;             // the last one's: BTC_OK, a negative BTC_ERR_*, or an OBEX response code (0xc4 not found ...)
    uint32_t length;        // bytes in its reply
    bool truncated;         // the reply outgrew the buffer; ask for fewer at a time
    int book_size;          // entries the phone says the book holds, or BTC_UNKNOWN
    int new_missed;         // missed calls not yet seen, or BTC_UNKNOWN
} btc_pb_status_t;

// `max_bytes` caps the buffer one reply is gathered in.
int btc_pb_start(size_t max_bytes);
void btc_pb_stop(void);
bool btc_pb_started(void);
// Call a phone's PBAP server: NULL picks the hands-free unit's phone, then
// the sink's sender, then the first paired device. The phone may ask its
// user to allow access first.
int btc_pb_connect(const uint8_t *addr);
int btc_pb_disconnect(void);

// Pull a whole book as vCards: `offset` and `count` page it (count 0 asks
// for the size alone), `format` 0 is vCard 2.1 and 1 is 3.0, `properties`
// the BTC_PB_PROP_* wanted. `path` names the file: "telecom/pb.vcf".
int btc_pb_pull(const char *path, uint16_t offset, uint16_t count, uint8_t format, uint64_t properties);
// The listing of a book's folder ("telecom/pb"): one handle and name per
// entry, as PBAP's XML. `order` 0 indexed, 1 by name; with `search`, only
// entries whose name (`search_by` 0) or number (1) matches.
int btc_pb_list(const char *folder, uint16_t offset, uint16_t count, uint8_t order,
    const char *search, uint8_t search_by);
// One entry by its handle in a folder: "telecom/pb", "5.vcf".
int btc_pb_entry(const char *folder, const char *handle, uint8_t format, uint64_t properties);
// Copy the last reply, up to `size` bytes; returns how many.
size_t btc_pb_data(uint8_t *dst, size_t size);
// Let the last reply's buffer go.
void btc_pb_release(void);
void btc_pb_status(btc_pb_status_t *out);
uint32_t btc_pb_changes(void);

// ---- the tether: PAN user, the internet through a phone ---------------------
//
// The board joins a phone's Bluetooth tethering (its PAN network access
// point) over BNEP. The phone's frames reach the host's IP stack through a
// network interface the backend keeps: on BTstack with lwIP, an lwIP netif
// that asks the phone for an address by DHCP and becomes the default route
// while the link is up. Backends without a PAN user return BTC_ERR_UNSUPPORTED.
// Changes: BTC_CHANGED_TETHER.

typedef struct {
    bool started;
    bool connecting;
    bool connected;
    btc_addr_t peer;
    int result;             // the last connect's: BTC_OK or a negative BTC_ERR_*
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_dropped;    // frames the link had no room for
} btc_tether_status_t;

int btc_tether_start(void);
void btc_tether_stop(void);
bool btc_tether_started(void);
// Join a phone's tethering: NULL picks the last device connected, then the
// first paired one. BTC_CHANGED_TETHER says it came up or failed.
int btc_tether_connect(const uint8_t *addr);
int btc_tether_disconnect(void);
void btc_tether_status(btc_tether_status_t *out);
uint32_t btc_tether_changes(void);
// The host's network interface for the link (an lwIP `struct netif *`), or
// NULL where there is none.
void *btc_tether_netif(void);

// ---- the audio gateway: HFP AG, the board as the phone ----------------------
//
// A headset, a speaker or a car kit connects to the board as it would to a
// phone. The board reports a network, signal and battery, rings with a
// caller's number, and carries the call's audio both ways. Bluedroid runs
// one HFP role at a time: btc_ag_start() fails with BTC_ERR_STATE while the
// hands-free unit is started, and btc_hf_start() while the gateway is.
//
// The headset's answer, hang-up and redial act at once, as a phone's would;
// a number it dials becomes an outgoing call, and DTMF digits and commands
// the gateway doesn't know queue for the program.
// Changes: BTC_CHANGED_CONNECTION, BTC_CHANGED_CALL, BTC_CHANGED_AUDIO,
// BTC_CHANGED_PHONE (gains, voice recognition), BTC_CHANGED_REQUEST.

typedef enum {
    BTC_AG_OPERATOR = 0,    // what the headset is told the network is called
    BTC_AG_NUMBER,          // the board's own number, for the headset's AT+CNUM
    BTC_AG_CALLER,          // the current call's number
    BTC_AG_DIALED,          // the last number the headset dialed
    BTC_AG_TEXT_COUNT,
} btc_ag_text_t;

typedef struct {
    bool connected;         // the service level connection is up
    btc_addr_t peer;
    uint32_t peer_features; // the headset's HFP features
    int service;            // 0 or 1, as reported
    int signal;             // 0..5
    int roaming;            // 0 or 1
    int battery;            // 0..5
    int call;               // btc_call_t
    bool in_band_ring;      // ringing plays through the call's audio
    bool voice_recognition;
    bool nrec;              // the headset wants the board's echo cancelling on
    int speaker_gain;       // 0..15, BTC_UNKNOWN until the headset says
    int mic_gain;
    bool audio;
    uint32_t sample_rate;   // 8000 or 16000 while audio is up, else 0
    bool wideband;          // the headset offers mSBC
    uint32_t sent;          // frames sent to the headset
    uint32_t silent;        // of those, filled with silence for want of audio
} btc_ag_status_t;

// `ring_ms` sizes the buffer for the headset's microphone, and the queue
// for what the board sends it.
int btc_ag_start(uint32_t ring_ms);
void btc_ag_stop(void);
bool btc_ag_started(void);
int btc_ag_connect(const uint8_t *addr);    // NULL: the last headset, if paired
int btc_ag_disconnect(void);
int btc_ag_connect_audio(void);
int btc_ag_disconnect_audio(void);

// What the gateway reports; BTC_UNKNOWN keeps a value.
int btc_ag_set_indicators(int service, int signal, int roaming, int battery);
int btc_ag_set_text(btc_ag_text_t which, const char *text);   // operator or number
// Calls. Each moves the call state and tells the headset.
int btc_ag_ring(const char *number);        // an incoming call rings
int btc_ag_dial(const char *number);        // the board calls out: dialing
int btc_ag_alerting(void);                  // the far end rings
int btc_ag_answer(void);                    // the call is active (either way)
int btc_ag_hangup(void);                    // end or reject it
int btc_ag_set_in_band_ring(bool on);
int btc_ag_voice_recognition(bool on);
int btc_ag_set_speaker_gain(int gain);      // 0..15, sent to the headset
int btc_ag_set_mic_gain(int gain);
// The headset's DTMF digits since the last call, oldest first; returns how many.
size_t btc_ag_dtmf(char *dst, size_t size);

// The headset's microphone: frames at the call's rate, `channels` (1 or 2)
// wide, scaled by nothing. Silence while there's no audio; returns how many
// came from the headset.
size_t btc_ag_read(int16_t *dst, size_t frames, uint8_t channels);
// Queue 16-bit mono at the call's rate for the headset's speaker. Returns
// the frames taken, 0 without call audio.
size_t btc_ag_write(const int16_t *pcm, size_t frames);
uint32_t btc_ag_changes(void);
void btc_ag_status(btc_ag_status_t *out);
void btc_ag_stats(btc_ring_stats_t *out);
size_t btc_ag_text(btc_ag_text_t which, char *dst, size_t size);

// ---- messages: MAP client ----

// A phone's texts (and its email, where it offers them), over OBEX. Folders
// are paths from the phone's root: "telecom/msg/inbox". One request runs at
// a time; each one's reply gathers into a buffer of up to `max_bytes`, read
// with btc_msg_data() once `busy` drops.

// Listing filters.
#define BTC_MSG_UNREAD          1   // read_filter: unread only
#define BTC_MSG_READ            2   //              read only
#define BTC_MSG_NO_SMS_GSM      0x01    // type_filter: these bits leave a type out
#define BTC_MSG_NO_SMS_CDMA     0x02
#define BTC_MSG_NO_EMAIL        0x04
#define BTC_MSG_NO_MMS          0x08

// btc_msg_set_status() indicators.
#define BTC_MSG_STATUS_READ     0
#define BTC_MSG_STATUS_DELETED  1

typedef struct {
    bool started;
    bool connected;
    bool connecting;
    btc_addr_t peer;
    int instance;           // the message service in use, or BTC_UNKNOWN
    int instances;          // how many the phone offers
    uint32_t features;      // its MAP feature bits
    uint32_t types;         // its message types: 1 email, 2 SMS GSM, 4 SMS CDMA, 8 MMS, 16 IM
    bool busy;              // a request is running
    int result;             // the last one's: BTC_OK, a negative BTC_ERR_*, or 0x80 | an OBEX response code
    size_t length;          // bytes of reply gathered
    bool truncated;         // the reply outgrew max_bytes
    int listing_size;       // from the last listing, or BTC_UNKNOWN
    int new_message;        // the phone's new-message flag from the last listing, or BTC_UNKNOWN
    char mse_time[24];      // the phone's clock at the last listing, "20261108T101500+0000"
    char pushed[64];        // the handle the phone gave the last pushed message
    bool notifying;         // the phone agreed to report changes
    bool notify_connected;  // the phone's report link is up
    uint8_t events;         // reports waiting for btc_msg_event()
    uint32_t events_dropped;
} btc_msg_status_t;

int btc_msg_start(size_t max_bytes);
void btc_msg_stop(void);
bool btc_msg_started(void);
// Find the phone's message services and open one: `instance` by its ID, or
// -1 for the first with texts. NULL: HandsFree's phone, the sink's sender,
// or the first paired device. The link comes later; watch CONNECTION.
int btc_msg_connect(const uint8_t *addr, int instance);
int btc_msg_disconnect(void);
// The subfolders of `folder`, as x-obex/folder-listing XML.
int btc_msg_folders(const char *folder, uint16_t offset, uint16_t count);
// The messages in folder/name, as MAP-msg-listing XML; count 0 asks only for
// listing_size. A zero filter or mask leaves it out.
int btc_msg_list(const char *folder, const char *name, uint16_t offset, uint16_t count,
    uint8_t read_filter, uint8_t type_filter, uint32_t parameter_mask, uint8_t subject_length);
// One message by handle, as a bMessage.
int btc_msg_get(const char *handle, bool attachments, bool utf8);
int btc_msg_set_status(const char *handle, uint8_t indicator, bool value);
// Hand the phone a bMessage for folder/name ("telecom/msg", "outbox" sends
// it); `pushed` then holds its handle.
int btc_msg_push(const char *folder, const char *name, const uint8_t *bmsg, size_t len, bool utf8, bool retry);
// Ask the phone to check for new messages now.
int btc_msg_update_inbox(void);
// Ask the phone to report changes (or stop). Starts the report server here
// and its SDP record the first time.
int btc_msg_notify(bool on);
size_t btc_msg_data(uint8_t *dst, size_t size);
void btc_msg_release(void);
// The oldest waiting report, MAP-event-report XML, or 0 when none waits.
size_t btc_msg_event(uint8_t *dst, size_t size);
void btc_msg_status(btc_msg_status_t *out);
uint32_t btc_msg_changes(void);

#ifdef __cplusplus
}
#endif
