# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a tone through an I2S amplifier with audiobusio.

This follows CircuitPython's own I2SOut example: build one cycle of a
sine wave, wrap it in a RawSample, and let I2SOut loop it for a few
seconds.

Needs: an I2S amplifier such as a MAX98357A wired to the three pins below.
On a desktop build there is no amplifier; set SINK to a file name and the
audio is written there instead, as signed 16-bit stereo samples.

A board whose DAC is a codec (an ES8311, say) needs two more things: the
codec switched on before this runs (board_config.audio_power() on a
PyDevices board), and MAIN_CLOCK set to the pin that clocks it.

Run it:
    mpremote run audioif_simpletest.py

Tested on: the unix port of MicroPython, writing to a file, and a
Waveshare ESP32-P4-WIFI6-Touch-LCD-4B through its ES8311 codec (pins 12,
10 and 9, MAIN_CLOCK 13).
"""

import array
import math
import time

import audiobusio
import audiocore

# Change these to match your setup.
BIT_CLOCK = 12
WORD_SELECT = 10
DATA = 9
MAIN_CLOCK = None  # a codec's MCLK pin, or None for an amplifier
SINK = None  # desktop only: a file name such as "tone.raw"
FREQUENCY = 440
SAMPLE_RATE = 22050
SECONDS = 2

# Build one cycle of a sine wave as unsigned 16-bit samples.
length = SAMPLE_RATE // FREQUENCY
sine_wave = array.array("H", [0] * length)
for i in range(length):
    sine_wave[i] = int(math.sin(math.pi * 2 * i / length) * (2**15 - 1) + 2**15)
tone = audiocore.RawSample(sine_wave, sample_rate=SAMPLE_RATE)

# Open the I2S bus and loop the cycle.
options = {}
if MAIN_CLOCK is not None:
    options["main_clock"] = MAIN_CLOCK
if SINK:
    options["sink"] = SINK
i2s = audiobusio.I2SOut(BIT_CLOCK, WORD_SELECT, DATA, **options)
i2s.play(tone, loop=True)
print("Playing %d Hz for %d seconds" % (SAMPLE_RATE // length, SECONDS))
time.sleep(SECONDS)

# Stop, and release the pins.
i2s.stop()
i2s.deinit()
print("Done")
