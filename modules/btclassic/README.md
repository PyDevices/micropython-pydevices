# btclassic — Bluetooth Classic: a speaker and a hands-free unit

`import btclassic` gives a board Bluetooth Classic. Your phone or computer
lists the board under its hostname, pairs with it without a PIN, and plays to
it. Your program reads the decoded audio into any PCM output, shows everything
the phone says about what's playing, cover art included, and drives the
player. It can also follow the phone itself, as a car kit does: battery,
signal, carrier and calls, which it can answer and end, with the call's audio
both ways.

```python
import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

network.WLAN(network.STA_IF).active(False)  # the radio is Bluetooth's now
bt = btclassic.Bluetooth()                  # listed as network.hostname()
music = btclassic.AudioSink(bt)
speaker = board_peripherals.pcm_out(AudioFormat(44100, 2, 16))
buf = bytearray(512 * 4)                    # 512 stereo frames
while True:
    if speaker.space() >= len(buf):         # the speaker sets the pace
        music.readinto(buf)                 # silence until audio arrives
        speaker.write(buf)
    if music.changes() & btclassic.TRACK:
        print(music.title, "-", music.artist)
```

Pair a phone, play a song, and the board prints its title and artist each
time the track changes:

```text
XXOUTPUTXX
```

Phones send 44.1 kHz almost always. A program that should follow any rate
opens its output at `music.sample_rate` once the `FORMAT` bit comes, as
[`examples/btclassic_simpletest.py`](examples/btclassic_simpletest.py) does.

## The radio and its profiles

The module is shaped like `bluetooth.BLE`: one radio object, and an object
per profile that takes it.

| Object | What it does |
|---|---|
| `Bluetooth()` | The radio. `active()` turns it on and off; `config()` reads and sets `name`, `discoverable`, `connectable`, `pin` and `mac`; `paired()` and `unpair()` manage pairings. |
| `AudioSink(bt)` | Receives audio (A2DP sink) and talks to the sender's player (AVRCP): `readinto()`, the track, `cover`, `position`, `play()`, `next()`, `seek()`, the `volume` shared with the sender's slider, and more. |
| `HandsFree(bt)` | Follows a phone (HFP hands-free): `battery`, `signal`, `operator`, the `call` and its `caller`, `answer()`, `hangup()`, `dial()`, and the call's audio through its own `readinto()` and `write()`. |

Nothing calls back into Python. Each object's state is properties, and its
`changes()` returns a bit for each kind of state that changed since you last
asked, so a program polls it as often as it redraws. A profile closes with
`deinit()` and works as a context manager.

Everything keeps running across a soft reset: making the objects again picks
up the running ones, links and all.

When `AudioSink` connects to a phone, `HandsFree` follows it there. The
phone decides when a call's audio comes to the board: for a call while the
board is its audio device, and for a voice or video chat app. While
`HandsFree.audio` is True, play `HandsFree.readinto()` instead of the music
(at `HandsFree.sample_rate`, 8 or 16 kHz) and send your microphone with
`HandsFree.write()`. The type stub, [`btclassic.pyi`](btclassic.pyi), has
every property and method.

## Before you start

Wi-Fi and Bluetooth share the ESP32's one radio. With Wi-Fi connected, audio
arrives slower than it plays and in bursts, so the speaker runs dry; turn
Wi-Fi off while listening, as the examples do.

Classic ESP32 only for now: it's the chip in this family with Bluetooth
Classic and MicroPython support. The module is opt-in (`--modules btclassic`,
or `--modules all,btclassic`) because it turns ESP-IDF's Bluedroid on in
place of NimBLE. ESP-IDF runs one Bluetooth host, so a firmware with
btclassic has no `bluetooth` (BLE) module. A board with PSRAM is the
comfortable fit: the Bluetooth stack's buffers go there.

## How it's built

[`esp-idf/btclassic`](esp-idf/btclassic) is the C that does the work, with
no MicroPython in it, one file per part: the radio, the A2DP sink, the AVRCP
controller with cover art, the HFP hands-free unit with call audio over HCI
(Bluedroid codes CVSD and mSBC), and the audio ring they play through. They
sit behind one header, [`btclassic.h`](esp-idf/btclassic/include/btclassic.h),
which another Bluetooth host could implement as well.
[`src/mod_btclassic.c`](src/mod_btclassic.c) wraps that header and nothing
else, and [`micropython.cmake`](micropython.cmake) compiles both as an
ordinary user C module, so a stock MicroPython esp32 build takes it with
`USER_C_MODULES`. Two settings go with it: [`sdkconfig.esp32`](sdkconfig.esp32)
in the board's `SDKCONFIG_DEFAULTS` turns Bluedroid, A2DP, AVRCP cover art and
HFP on, and `micropython.cmake` turns the `bluetooth` module off itself.
`build_mp.py` does the first for you.

The same folder is also an ESP-IDF component of its own. CI builds it in a
plain ESP-IDF 6.1 app for the ESP32-S31, which has Bluetooth Classic too, so
it is ready for that chip once MicroPython supports it.
