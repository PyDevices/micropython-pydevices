# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play your phone through the board's speaker.

The board shows up in your phone's or computer's Bluetooth list under its
hostname. Pair with it and play something: the board plays it through its
speaker and prints each new title, with the left and right levels once a
second.

Needs: a classic ESP32 board with a speaker or I2S amplifier, firmware built
with the btclassic module, and a board config with a pcm_out role.

Run it:

    mpftp run btclassic_simpletest.py

Tested on: Adafruit QT Py ESP32 Pico with the Audio BFF, MicroPython
1.30.0-preview, playing from a Windows 11 PC.
"""

import time

import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

# Change these to match your setup.
FRAMES = 512  # frames per write; 512 is about 12 ms at 44.1 kHz

# Wi-Fi and Bluetooth share the ESP32's radio, and with Wi-Fi up the audio
# arrives slower than it plays, in bursts. Turn Wi-Fi off while listening.
network.WLAN(network.STA_IF).active(False)

# Start Bluetooth and the audio sink. The board appears under its hostname.
bt = btclassic.Bluetooth()
receiver = btclassic.AudioSink(bt)
print("Pair with %s and play something." % network.hostname())
buf = bytearray(FRAMES * 4)
speaker = None
rate = 0
last = time.ticks_ms()

while True:
    # Open the speaker at the stream's rate, and again if the rate changes.
    if receiver.sample_rate and receiver.sample_rate != rate:
        rate = receiver.sample_rate
        if speaker:
            speaker.close()
        speaker = board_peripherals.pcm_out(AudioFormat(rate, 2, 16))

    # Move one block from Bluetooth to the speaker whenever the speaker has
    # room for it, so the speaker's clock sets the pace. Neither side waits:
    # read faster than the speaker plays and its queue drops the rest.
    if speaker and speaker.space() >= len(buf):
        receiver.readinto(buf)
        speaker.write(buf)
    else:
        time.sleep_ms(5)

    # Say what's playing when the track changes.
    if receiver.changes() & btclassic.TRACK and receiver.title:
        print("Playing:", receiver.title, "-", receiver.artist)

    # Print the levels once a second.
    if time.ticks_diff(time.ticks_ms(), last) >= 1000:
        last = time.ticks_ms()
        print("levels", receiver.levels())
