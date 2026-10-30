# picotts — text to speech on the board

`import picotts` makes a board with PSRAM speak, with no network and no PC.
It is the SVOX Pico engine, an en-US voice at 16 kHz that handles numbers,
abbreviations and dates, behind the API of Adafruit's
[`adafruit_picotts`](https://github.com/adafruit/Adafruit_CircuitPython_PicoTTS),
so [Adafruit's text-to-speech guide](https://learn.adafruit.com/circuitpython-text-to-speech)
applies with two changes: the import name, and the audio output you hand it.

```python
import board_peripherals
import picotts
from audiodev import AudioFormat

audio = board_peripherals.audio_out(AudioFormat(16000, 1, 16))
tts = picotts.TTS(audio)
tts.say("Doctor Smith read 1,234 pages on the 1st of May.")
```

[`examples/picotts_simpletest.py`](examples/picotts_simpletest.py) says three
sentences and prints how long each took.

## What you need

- **A board with PSRAM.** The engine's work area is 1.1 MB and the voice
  1.43 MB, about 2.5 MB together. On a board without PSRAM, `TTS()` raises
  `MemoryError` saying so before it allocates anything. An ESP32-S3 or P4
  with 8 MB or more is comfortable.
- **Firmware built with picotts.** It is opt-in, so `all` leaves it out:

  ```bash
  ./build_mp.py --port esp32 --board ESP32_GENERIC_S3 --variant SPIRAM_OCT --modules all,picotts
  ```

  It adds about 160 KB of engine code to the image. MicroPython only: on
  CircuitPython, use Adafruit's library, which is the same engine as a
  native `.mpy`.
- **The voice files in `/lib/picotts/`.** Two files, `en_US_ta.bin`
  (650 KB) and `en_US_lh0_sg.bin` (777 KB). With the board on Wi-Fi, `mip`
  fetches them from Adafruit's repository, at a pinned commit:

  ```python
  import mip
  mip.install("github:PyDevices/micropython-pydevices/modules/picotts/voices/en-US.json")
  ```

  Or download the two files from the URLs in
  [`voices/en-US.json`](voices/en-US.json) and copy them to `/lib/picotts/`
  with `mpremote cp` or `mpftp put`. `TTS(audio, voice_dir="/sd/voice")`
  reads them from another folder.
- **An audio output at 16,000 Hz.** Speech comes out at 16 kHz mono, and
  `AudioOut` doesn't resample, so open it at that rate or the voice plays at
  the wrong pitch. `TTS` also accepts `audiobusio.I2SOut`, or a voice of your
  own `audiomixer.Mixer` running at 16 kHz.

## Why the voice is a file

The voice lives on the filesystem, not in its own flash partition and not in
the firmware image. A partition would change the board's partition table,
which moves or shrinks the filesystem, and every board's table differs. In the
image it would add 1.43 MB to an app slot that is often nearly full. As files
it installs like any package, the firmware stays the same for every language,
and nothing about the board changes. The cost is that `TTS()` reads the files
into PSRAM, which takes a moment once, and the engine reads them there.

## API

The same as `adafruit_picotts`: `TTS(audio, *, voice_dir=None)`, then
`say(text, *, wait=True, interrupt=False)`, `update()` after
`say(..., wait=False)`, `stop()`, `deinit()`, the `rate` (words per minute,
default 150), `pitch` (0.5 to 2.0), `volume` (0.0 to 1.0) and `voice`
properties, `voices`, `speaking` and `sample_rate`. `voice_dir` is the one
addition.

Underneath is the C module `_picotts`: `Engine(ta, sg, memory)` renders text
to samples with `start(text)`, `render(buffer)`, `speaking`, `stop()` and
`deinit()`, for when you want the samples themselves (to save a WAV, or feed
your own player). [`tests/test_picotts.py`](tests/test_picotts.py) uses it
that way.

## Licence

The engine is SVOX AG's, under the Apache License 2.0, and so are the voice
files. [NOTICE](NOTICE) lists where every file came from;
[third_party/svox/ORIGIN.txt](third_party/svox/ORIGIN.txt) lists every change
made to the engine on its way here.
