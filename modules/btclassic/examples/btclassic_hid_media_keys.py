# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Press media keys on a phone or computer over Bluetooth.

The board becomes a Bluetooth keyboard with media keys, the kind a phone
takes from a remote or a headset button. Each time a phone or computer
connects, the board presses the keys in KEYS one after another, and its
volume or player follows.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W).

Run it:

    mpftp run btclassic_hid_media_keys.py

Pair your phone or computer with the board's hostname. Its volume steps
down three times.

Tested on:
"""

import time

import btclassic
import network

# HID Consumer usages for the usual media keys.
PLAY_PAUSE = 0xCD
NEXT = 0xB5
PREVIOUS = 0xB6
VOLUME_UP = 0xE9
VOLUME_DOWN = 0xEA
MUTE = 0xE2

# Change these to match your setup.
KEYS = (VOLUME_DOWN, VOLUME_DOWN, VOLUME_DOWN)  # pressed in turn
GAP_MS = 1000  # between presses

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Start a keyboard; its descriptor carries the media keys too.
bt = btclassic.Bluetooth()
remote = btclassic.HIDDevice(bt, btclassic.HIDDevice.KEYBOARD)
print("Pair with %s from your phone or computer." % network.hostname())

# Call back the phone or computer it last connected to, if there is one.
try:
    remote.connect()
except OSError:
    pass

while True:
    # When a host connects, give it a moment, then press each key.
    if remote.changes() & btclassic.CONNECTION:
        print("Connected" if remote.connected else "Disconnected")
        if remote.connected:
            time.sleep(2)
            for usage in KEYS:
                print("Key", hex(usage), "sent" if remote.consumer(usage) else "not sent")
                time.sleep_ms(GAP_MS)
    time.sleep_ms(50)
