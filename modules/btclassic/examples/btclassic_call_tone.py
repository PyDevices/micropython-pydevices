# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Send a tone into a phone call and measure the caller's voice.

The board is a hands-free unit for a phone (HFP). It answers an incoming
call, and while the call's audio is on the board, your program has both
directions as samples: it sends a tone where a microphone would go, and
reads the caller's voice to print how loud it is, once a second. This is
the way in for a program that processes call audio itself;
btclassic_speakerphone.py plays a call with no Python in the path.

Needs: a classic ESP32 with firmware built with the btclassic module (a
Pico W's Bluetooth can't carry call audio), and a phone, or another board
running btclassic_audio_gateway.py.

Run it:

    mpftp run btclassic_call_tone.py

Pair your phone with the board's hostname and call it. The caller hears
the tone.

Tested on:
"""

import math
import time
from array import array

import btclassic
import network

# Change these to match your setup.
FREQUENCY = 1000  # Hz, the tone sent into the call
LEVEL = 8000  # its peak, out of 32767

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for calls.
network.WLAN(network.STA_IF).active(False)

# Start the hands-free unit. The phone connects to it once paired.
bt = btclassic.Bluetooth()
phone = btclassic.HandsFree(bt)
print("Pair with %s, then call it." % bt.config("name"))

start = None
while True:
    changed = phone.changes()
    if changed & btclassic.CONNECTION:
        print("Connected" if phone.connected else "Disconnected")

    # Answer an incoming call.
    if changed & btclassic.CALL:
        print("Call:", phone.call, phone.caller)
        if phone.call == "incoming":
            phone.answer()

    # When the call's audio comes up, make a 20 ms block of the tone, and
    # a buffer as long for the caller's voice.
    if changed & btclassic.AUDIO:
        if phone.audio:
            rate = phone.sample_rate
            print("Call audio at", rate, "Hz")
            tone = array("h", (int(LEVEL * math.sin(2 * math.pi * FREQUENCY * i / rate)) for i in range(rate // 50)))
            voice = array("h", tone)
            blocks = 0
            start = time.ticks_ms()
        else:
            print("Call audio off the board;", phone.stats())
            start = None

    # Send a block whenever the clock says one is due, and read one.
    if start is not None and blocks * 20 < time.ticks_diff(time.ticks_ms(), start) + 100:
        phone.write(tone)
        phone.readinto(voice)
        blocks += 1
        if blocks % 50 == 0:
            print("Caller's peak", max(abs(s) for s in voice))
    time.sleep_ms(5)
