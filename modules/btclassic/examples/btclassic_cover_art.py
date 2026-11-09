# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Save the cover art of what your phone plays.

The board connects to a phone as a Bluetooth speaker does. Each time the
track changes, the phone sends its cover, a 200 by 200 JPEG, and the board
saves it to its filesystem and prints the title and the file's size. The
cover comes into one buffer the program keeps, so each new one makes no
garbage.

The board doesn't play the music here; btclassic_speaker.py does.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a phone whose music app sends cover
art over Bluetooth (most do).

Run it:

    mpftp run btclassic_cover_art.py
    mpftp cp :cover.jpg .

Tested on:
"""

import time

import btclassic
import network

# Change these to match your setup.
FILENAME = "cover.jpg"
MAX_BYTES = 65536  # the largest cover kept; they are a few tens of KB

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Start the sink. A phone the board knows is called back.
bt = btclassic.Bluetooth()
music = btclassic.AudioSink(bt)
print("Pair with %s and play something." % bt.config("name"))
buf = bytearray(MAX_BYTES)

while True:
    changed = music.changes()
    if changed & btclassic.CONNECTION:
        print("Connected to", music.peer_name if music.connected else "nothing")
    if changed & btclassic.TRACK and music.title:
        print("Track:", music.title, "-", music.artist)

    # A new cover arrived, or the track has none.
    if changed & btclassic.COVER:
        if music.cover_size > len(buf):
            print("Cover of %d bytes is larger than MAX_BYTES" % music.cover_size)
        else:
            size = music.cover_into(buf)
            if size == 0:
                print("This track has no cover")
            else:
                # Save it. A JPEG starts with FF D8 and ends with FF D9.
                with open(FILENAME, "wb") as f:
                    f.write(memoryview(buf)[:size])
                whole = buf[:2] == b"\xff\xd8" and buf[size - 2 : size] == b"\xff\xd9"
                print("Saved %s: %d bytes, %s" % (FILENAME, size, "a JPEG" if whole else "not a JPEG"))
    time.sleep_ms(100)
