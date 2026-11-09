# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Follow a phone's signal, battery and calls, and answer them.

The board follows a phone as a car kit does (HFP hands-free), without the
call's audio. It prints the phone's network, signal and battery, and each
call as it rings, connects and ends, with the caller's number. It answers
an incoming call, sends a few keypad tones, and hangs up after HANG_UP_S
seconds. With NUMBER set, it has the phone dial that number first.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a phone. Another board running
btclassic_audio_gateway.py can play the phone.

Run it:

    mpftp run btclassic_phone_status.py

Pair your phone with the board's hostname, then have someone call you.

Tested on: Adafruit QT Py ESP32 Pico, MicroPython 1.30.0-preview with
btclassic, called by an ESP32 Audio Kit running btclassic_audio_gateway.py.
"""

import time

import btclassic
import network

# Change these to match your setup.
NUMBER = None  # a number for the phone to dial once connected, or None
DIGITS = "123#"  # keypad tones sent once a call is up
HANG_UP_S = 10  # seconds a call lasts before the board ends it

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Start the hands-free unit. The phone connects to it once paired.
bt = btclassic.Bluetooth()
phone = btclassic.HandsFree(bt)
print("Pair with %s, then call it." % bt.config("name"))

started = None
while True:
    changed = phone.changes()

    # When the phone connects, dial if asked to.
    if changed & btclassic.CONNECTION:
        print("Connected" if phone.connected else "Disconnected")
        if phone.connected and NUMBER:
            phone.dial(NUMBER)

    # Print the phone's state as it changes.
    if changed & btclassic.PHONE:
        print(
            "Network %s (%s), signal %s/5, battery %s/5"
            % (phone.operator or "?", "up" if phone.service else "down", phone.signal, phone.battery)
        )

    # Follow the call: answer it, send the tones once it's up.
    if changed & btclassic.CALL:
        print("Call:", phone.call, phone.caller)
        if phone.call == "incoming":
            phone.answer()
        elif phone.call == "active" and started is None:
            started = time.ticks_ms()
            phone.dtmf(DIGITS)
            print("Sent tones", DIGITS)
        elif phone.call == "idle":
            started = None

    # End the call when its time is up.
    if started is not None and time.ticks_diff(time.ticks_ms(), started) > HANG_UP_S * 1000:
        print("Hanging up")
        phone.hangup()
        started = None
    time.sleep_ms(100)
