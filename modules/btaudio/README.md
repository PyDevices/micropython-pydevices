# btaudio — Bluetooth audio in, as a speaker

`import btaudio` makes the board a Bluetooth speaker's receiving half. Your
phone or computer lists the board under its hostname, pairs with it without a
PIN, and plays to it. Your program reads the decoded audio into any PCM output,
shows what's playing, and sends play, pause and skip back.

```python
import board_peripherals
import btaudio
from audiodev import AudioFormat

receiver = btaudio.Receiver()           # appears as network.hostname()
speaker = board_peripherals.pcm_out(AudioFormat(44100, 2, 16))
buf = bytearray(512 * 4)                # 512 stereo frames
while True:
    receiver.readinto(buf)              # silence until audio arrives
    speaker.write(buf)
    if receiver.changes() & btaudio.TRACK:
        print(receiver.title, "-", receiver.artist)
```

Phones send 44.1 kHz almost always. A program that should follow any rate
opens its output at `receiver.sample_rate` once the `FORMAT` bit comes, as
[`examples/btaudio_simpletest.py`](examples/btaudio_simpletest.py) does.

Classic ESP32 only for now: it's the chip in this family with Bluetooth
Classic and MicroPython support. The module is opt-in (`--modules btaudio`, or
`--modules all,btaudio`) because it turns ESP-IDF's Bluedroid on in place of
NimBLE. ESP-IDF runs one Bluetooth host, so a firmware with btaudio has no
`bluetooth` (BLE) module. A board with PSRAM is the comfortable fit: the
Bluetooth stack's buffers go there.

## API

### `btaudio.Receiver(name=None, *, buffer_ms=500)`

Starts Bluetooth and makes the board discoverable as `name`, by default the
board's hostname. There is one receiver: a second `Receiver()` takes over the
running one, link and all, which is how a program picks it up again after a
soft reset. `buffer_ms` (50 to 5000) is how much audio it holds; playback
starts once half of it has arrived, so it is also the delay to the speaker.

`readinto(buf)` fills `buf` with 16-bit stereo frames (4 bytes each) at
`sample_rate`, scaled by `volume`, and never waits: whatever the stream can't
fill is silence. It returns how many frames came from the stream. Your
output's clock and the sender's never quite agree, so the receiver keeps its
buffer near half full by dropping or repeating one frame now and then.

| Read | What it is |
|---|---|
| `connected`, `streaming`, `playing` | A sender is connected; audio is flowing; the sender says it is playing. |
| `sample_rate` | The stream's rate in Hz, 0 before the first stream. |
| `volume` | 0 to 127, shared with the sender's slider: set it here and the slider moves. |
| `title`, `artist`, `album` | What's playing, or `""` if the sender hasn't said. |
| `peer` | The sender's Bluetooth address, `""` with none connected. |

`changes()` returns a bit for each of `CONNECTION`, `STREAM`, `FORMAT`,
`TRACK`, `PLAYING` and `VOLUME` that changed since the last call. `levels()`
returns the loudest left and right sample since the last call (0 to 32767),
for a meter. `stats()` returns the buffer's counters.

`play()`, `pause()`, `next()` and `previous()` drive the sender.
`disconnect()` drops it, and `close()` shuts Bluetooth down.

The type stub, [`btaudio.pyi`](btaudio.pyi), has the same in full.

## How it's built

[`components/btaudio`](components/btaudio) is a plain ESP-IDF component with
no MicroPython in it: A2DP sink, AVRCP and the buffer, on the public Bluedroid
API. [`src/mod_btaudio.c`](src/mod_btaudio.c) wraps it.
[`sdkconfig.esp32`](sdkconfig.esp32) turns Bluedroid, A2DP and AVRCP on, and
[`mpconfigboard.esp32.h`](mpconfigboard.esp32.h) turns the `bluetooth` module
off. CI also builds the component on its own with ESP-IDF 6.1 for the
ESP32-S31, which has Bluetooth Classic too, so it is ready for that chip once
MicroPython supports it.
