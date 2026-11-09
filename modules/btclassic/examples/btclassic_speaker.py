# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Make the board a Bluetooth speaker that plays on its own core.

In a firmware with audiodsp, the AudioSink is an audiosample, like a synth
or an MP3 decoder. The board's audio_out hands it to the audio pump, which
pulls it in C on its own task, so nothing your program does can make the
music stutter. The loop is left to print each new title and the levels.

Needs: a classic ESP32 board with PSRAM, firmware built with the btclassic
and audiodsp modules, and a board config whose audio_out drives I2S.

Run it:

    mpftp run btclassic_speaker.py

Pair your phone with the board's hostname and play something. The board
prints the title and artist of each track, and the levels once a second.

Tested on: ESP32 Audio Kit (ESP32-A1S), MicroPython 1.30.0-preview,
playing from a Galaxy S21.
"""

import time

import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

# Change these to match your setup.
BUFFER_MS = 1000  # what the board holds; a phone sends a burst as it starts
NAME = None  # the name the phone lists, or None for the hostname

# Wi-Fi and Bluetooth share the ESP32's radio. Turn Wi-Fi off for audio.
network.WLAN(network.STA_IF).active(False)

# Start Bluetooth and the audio sink. A phone the board knows is called back.
bt = btclassic.Bluetooth()
if NAME:
    bt.config(name=NAME)
music = btclassic.AudioSink(bt, buffer_ms=BUFFER_MS)
print("Pair with %s and play something." % bt.config("name"))

speaker = None
rate = 0
last = time.ticks_ms()
while True:
    # Open the speaker at the stream's rate, and again if the rate changes.
    # From then on the pump plays the music; this loop doesn't touch it.
    if music.sample_rate and music.sample_rate != rate:
        rate = music.sample_rate
        if speaker:
            speaker.stop()
            speaker.close()
        speaker = board_peripherals.audio_out(AudioFormat(rate, music.channel_count, 16))
        speaker.play(music)
        print("Playing at", rate, "Hz")

    # Say what's playing when the track changes.
    if music.changes() & btclassic.TRACK and music.title:
        print("Now playing:", music.title, "-", music.artist)

    # Print the levels once a second while music plays.
    if music.playing and time.ticks_diff(time.ticks_ms(), last) >= 1000:
        last = time.ticks_ms()
        print("levels", music.levels())
    time.sleep_ms(100)
