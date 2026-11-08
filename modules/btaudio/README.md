# btaudio — Bluetooth audio in, as a speaker

`import btaudio` makes the board a Bluetooth speaker's receiving half. Your
phone or computer lists the board under its hostname, pairs with it without a
PIN, and plays to it. Your program reads the decoded audio into any PCM output,
shows everything the phone says about what's playing, cover art included, and
drives the player. It also follows the phone itself: battery, signal, carrier
and calls, which it can answer and end, with the call's audio heard on the
board and its microphone sent back.

```python
import board_peripherals
import btaudio
import network
from audiodev import AudioFormat

network.WLAN(network.STA_IF).active(False)  # the radio is Bluetooth's now
receiver = btaudio.Receiver()           # appears as network.hostname()
speaker = board_peripherals.pcm_out(AudioFormat(44100, 2, 16))
buf = bytearray(512 * 4)                # 512 stereo frames
while True:
    if speaker.space() >= len(buf):     # the speaker sets the pace
        receiver.readinto(buf)          # silence until audio arrives
        speaker.write(buf)
    if receiver.changes() & btaudio.TRACK:
        print(receiver.title, "-", receiver.artist)
```

Phones send 44.1 kHz almost always. A program that should follow any rate
opens its output at `receiver.sample_rate` once the `FORMAT` bit comes, as
[`examples/btaudio_simpletest.py`](examples/btaudio_simpletest.py) does.

Wi-Fi and Bluetooth share the ESP32's one radio. With Wi-Fi connected, audio
arrives slower than it plays and in bursts, so the speaker runs dry; turn
Wi-Fi off while listening (`network.WLAN(network.STA_IF).active(False)`), as
the examples do.

Classic ESP32 only for now: it's the chip in this family with Bluetooth
Classic and MicroPython support. The module is opt-in (`--modules btaudio`, or
`--modules all,btaudio`) because it turns ESP-IDF's Bluedroid on in place of
NimBLE. ESP-IDF runs one Bluetooth host, so a firmware with btaudio has no
`bluetooth` (BLE) module. A board with PSRAM is the comfortable fit: the
Bluetooth stack's buffers go there.

## API

### `btaudio.Receiver(name=None, *, buffer_ms=500, reconnect=True)`

Starts Bluetooth and makes the board discoverable as `name`, by default the
board's hostname. There is one receiver: a second `Receiver()` takes over the
running one, link and all, which is how a program picks it up again after a
soft reset. `buffer_ms` (50 to 5000) is how much audio it holds; playback
starts once half of it has arrived, so it is also the delay to the speaker.
With `reconnect`, it calls back the last sender if that sender is still
paired, the way a speaker does when it's switched on.

`readinto(buf)` fills `buf` with 16-bit stereo frames (4 bytes each) at
`sample_rate`, scaled by `volume`, and never waits: whatever the stream can't
fill is silence. So the output sets the pace: read a block when it has room
for one (`space()` on a board's `pcm_out`), not in a loop of its own. It returns how many frames came from the stream. Your
output's clock and the sender's never quite agree, so the receiver keeps its
buffer near half full by dropping or repeating one frame now and then.

| Read | What it is |
|---|---|
| `connected`, `streaming`, `playing` | A sender is connected; audio is flowing; the sender says it is playing. |
| `sample_rate` | The stream's rate in Hz, 0 before the first stream. |
| `volume` | 0 to 127, shared with the sender's slider: set it here and the slider moves. |
| `status` | `"playing"`, `"paused"`, `"stopped"`, `"seeking forward"`, `"seeking back"` or `"error"`. |
| `title`, `artist`, `album`, `genre` | What's playing, or `""` if the sender hasn't said. |
| `track_number`, `track_count` | Its place on the album, and the album's length. |
| `duration`, `position` | In milliseconds. `position` moves on between the sender's reports, so read it as often as you redraw. |
| `cover` | The cover art as a 200 by 200 JPEG, or `None`. |
| `peer`, `peer_name` | The sender's Bluetooth address and device name. |

A value the sender hasn't reported reads `None`, and not every sender reports
everything.

`changes()` returns a bit for each of `CONNECTION`, `STREAM`, `FORMAT`,
`TRACK`, `PLAYING`, `VOLUME`, `POSITION`, `PEER`, `COVER`, `PHONE` and `CALL`
that changed since the last call. `levels()` returns the loudest left and
right sample since the last call (0 to 32767), for a meter. `stats()` returns
the buffer's counters.

`play()`, `pause()`, `stop()`, `next()` and `previous()` drive the sender.
`seek(1)` holds fast forward and `seek(-1)` rewind until `seek(0)`.
`key(code)` sends any other AVRCP key, such as `btaudio.KEY_MUTE`.
`setting("repeat", "all")` and its kin (`equalizer`, `shuffle`, `scan`) ask
the player to change mode; many ignore it, and none can be read back.
`disconnect()` drops the sender, and `close()` shuts Bluetooth down.

`link()` returns the connection as a dict: the sender's name, its signal and
battery, the codec and its settings, and the delay reported to the sender.
`phone()` returns the phone's side over the hands-free link: battery,
signal, carrier, roaming, and the call's state and number. `answer()`,
`hangup()`, `dial()`, `voice()` and `dtmf()` act on calls.

While a call's audio is up, `call_audio` is True, `FORMAT` changes and
`sample_rate` becomes the call's (8000, or 16000 for wideband). `readinto()`
then returns the phone's voice instead of music, so a player hears calls
without a line of its own, once it reopens its output at the new rate.
`write(buf)` sends the microphone back, 16-bit mono at that rate. A call
keeps 200 ms in the buffer rather than half of `buffer_ms`, so the
conversation doesn't lag.

The type stub, [`btaudio.pyi`](btaudio.pyi), has the same in full.

## How it's built

[`esp-idf/btaudio`](esp-idf/btaudio) is the C that does the work, with no
MicroPython in it: A2DP sink, AVRCP with cover art, the HFP hands-free
client with call audio over HCI (Bluedroid codes CVSD and mSBC) and the
buffer, on the public Bluedroid API. [`src/mod_btaudio.c`](src/mod_btaudio.c) wraps it, and
[`micropython.cmake`](micropython.cmake) compiles both as an ordinary user C
module, so a stock MicroPython esp32 build takes it with `USER_C_MODULES`.
Two settings go with it: [`sdkconfig.esp32`](sdkconfig.esp32) in the board's
`SDKCONFIG_DEFAULTS` turns Bluedroid, A2DP, AVRCP cover art and HFP on, and
`micropython.cmake` turns the `bluetooth` module off itself. `build_mp.py`
does the first for you.

The same folder is also an ESP-IDF component of its own. CI builds it in a
plain ESP-IDF 6.1 app for the ESP32-S31, which has Bluetooth Classic too, so it
is ready for that chip once MicroPython supports it.
