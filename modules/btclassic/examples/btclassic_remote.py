# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Drive a phone's music player from the board.

The board connects to a phone as a Bluetooth speaker does, then works the
phone's player like a remote: play, skip to the next track, set the
volume, turn repeat on, and pause. It prints what the phone says back, the
track, its position, the play status and the volume, and the link's codec
and signal.

The board doesn't play the music here; btclassic_speaker.py does.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a phone with a music app.

Run it:

    mpftp run btclassic_remote.py

Pair your phone with the board's hostname and open a music app. The board
plays, skips, turns the volume to VOLUME and pauses, a few seconds apart.

Tested on: ESP32 Audio Kit, MicroPython 1.30.0-preview with btclassic,
with a Galaxy S21 playing Spotify.
"""

import time

import btclassic
import network

# Change these to match your setup.
VOLUME = 64  # 0 to 127, shared with the phone's slider
STEP_MS = 5000  # between steps

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Start the sink. A phone the board knows is called back.
bt = btclassic.Bluetooth()
music = btclassic.AudioSink(bt)
print("Pair with %s and open a music app." % bt.config("name"))

# The remote's steps, in order, once the phone is connected.
steps = [
    ("play", music.play),
    ("next track", music.next),
    ("volume", lambda: setattr(music, "volume", VOLUME)),
    ("repeat all", lambda: music.setting("repeat", "all")),
    ("pause", music.pause),
]
due = None

while True:
    changed = music.changes()

    if changed & btclassic.CONNECTION:
        print("Connected to", music.peer_name if music.connected else "nothing")

    # Once the phone is connected, print the link and start the steps.
    if music.connected and due is None:
        link = music.link()
        print("Codec %s, %s Hz, signal %s dB" % (link["codec"], link["sample_rate"], link["rssi_delta"]))
        due = time.ticks_add(time.ticks_ms(), STEP_MS)

    # Print what the phone reports.
    if changed & btclassic.TRACK:
        print("Track: %s - %s (%s ms)" % (music.title, music.artist, music.duration))
    if changed & btclassic.PLAYING:
        print("Status:", music.status)
    if changed & btclassic.VOLUME:
        print("Volume:", music.volume)
    if changed & btclassic.POSITION and music.position is not None:
        print("Position: %d s" % (music.position // 1000))

    # Take the next step when it's due.
    if steps and due is not None and time.ticks_diff(time.ticks_ms(), due) >= 0:
        name, action = steps.pop(0)
        print(">", name)
        action()
        due = time.ticks_add(time.ticks_ms(), STEP_MS)
    time.sleep_ms(100)
