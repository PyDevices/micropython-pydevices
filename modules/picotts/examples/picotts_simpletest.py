# SPDX-FileCopyrightText: 2026 PyDevices
#
# SPDX-License-Identifier: MIT
"""Speak a few sentences through the board's speaker.

Text to speech on the board itself, with no network: the SVOX Pico engine
renders 16 kHz speech, and the board's audio output plays it. The output is
opened at 16 kHz because it doesn't resample. Each sentence prints how long it
took to say.

Needs: a board with PSRAM and a speaker or I2S amplifier, firmware built with
the picotts module, a board config with an audio_out role, and the en-US voice
files in /lib/picotts/ (see the module README).

Run it:
    mpftp run picotts_simpletest.py

Tested on: LilyGO T-Embed (ESP32-S3, 8 MB PSRAM), MicroPython 1.30.0-preview.
"""

import time

import board_peripherals
import picotts
from audiodev import AudioFormat

# Change these to match your setup.
VOLUME = 0.6  # 0.0 to 1.0
SENTENCES = (
    "Hello from Micro Python.",
    "This is the S VOX Pico voice.",
    "Doctor Smith read 1,234 pages on the 1st of May.",
)

# Open the speaker at the engine's rate, 16 kHz mono.
audio = board_peripherals.audio_out(AudioFormat(16000, 1, 16))

# Load the engine and the voice (about 2.5 MB of PSRAM).
tts = picotts.TTS(audio)
tts.volume = VOLUME

# Say each sentence and print how long it took.
for text in SENTENCES:
    start = time.ticks_ms()
    tts.say(text)
    print("%s %.2f s" % (text, time.ticks_diff(time.ticks_ms(), start) / 1000))
    time.sleep(0.5)

tts.deinit()
