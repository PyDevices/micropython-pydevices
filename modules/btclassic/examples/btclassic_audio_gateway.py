# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Ring a Bluetooth headset, as a phone does, and talk to it.

The board plays the phone for a headset or car kit (HFP audio gateway).
It finds one in pairing mode, connects, shows it a network, signal and
battery, and rings it with a caller's number. When the headset answers,
the board sends it a tone and prints how loud its microphone is, once a
second, along with any keypad digits it sends. The call ends when the
headset hangs up, or after CALL_S seconds.

Needs: a classic ESP32 with firmware built with the btclassic module (a
Pico W's Bluetooth can't carry call audio), and a headset or car kit in
pairing mode. Another board running btclassic_phone_status.py or
btclassic_call_tone.py works as the headset.

Run it:

    mpftp run btclassic_audio_gateway.py

Tested on: ESP32 Audio Kit, MicroPython 1.30.0-preview with btclassic,
calling an Adafruit QT Py ESP32 Pico running btclassic_phone_status.py
and btclassic_call_tone.py.
"""

import math
import time
from array import array

import btclassic
import network

# Change these to match your setup.
ADDRESS = None  # the headset's 6-byte address, or None to look for one
CALLER = "5551234"  # the number the headset shows
FREQUENCY = 500  # Hz, the tone sent to the headset
CALL_S = 20  # seconds before the board ends the call

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for calls.
network.WLAN(network.STA_IF).active(False)
bt = btclassic.Bluetooth()
bt.active(True)

# Look for a headset. Major class 4 is audio and video.
address = ADDRESS
if address is None:
    print("Looking for a headset in pairing mode...")
    bt.inquiry(10240)
    while bt.inquiring() and address is None:
        time.sleep_ms(200)
        for addr, name, cod, rssi in bt.found():
            if (cod >> 8) & 0x1F == 4:
                print("Found", name or addr.hex(":"))
                address = addr
                break
    bt.inquiry(0)
if address is None:
    raise SystemExit("No headset found.")

# Connect, and fill in what the headset's display shows.
gateway = btclassic.AudioGateway(bt)
gateway.connect(address)
while not gateway.connected:
    time.sleep_ms(100)
gateway.operator = "MicroPython"
gateway.signal = 4
gateway.battery = 5
print("Connected; ringing")
gateway.ring(CALLER)

start = None
while True:
    changed = gateway.changes()
    if changed & btclassic.CALL:
        print("Call:", gateway.call)
        if gateway.call == "idle":
            break
    if changed & btclassic.REQUEST:
        print("Keypad:", gateway.dtmf())

    # When the call's audio comes up, make a 20 ms block of the tone, and
    # a buffer as long for the microphone.
    if changed & btclassic.AUDIO and gateway.audio:
        rate = gateway.sample_rate
        print("Call audio at", rate, "Hz")
        tone = array("h", (int(8000 * math.sin(2 * math.pi * FREQUENCY * i / rate)) for i in range(rate // 50)))
        mic = array("h", tone)
        blocks = 0
        start = time.ticks_ms()

    # Send a block whenever the clock says one is due, and read one.
    if gateway.audio and start is not None and blocks * 20 < time.ticks_diff(time.ticks_ms(), start) + 100:
        gateway.write(tone)
        gateway.readinto(mic)
        blocks += 1
        if blocks % 50 == 0:
            print("Microphone peak", max(abs(s) for s in mic))

    # End the call when its time is up.
    if start is not None and time.ticks_diff(time.ticks_ms(), start) > CALL_S * 1000:
        gateway.hangup()
        start = None
    time.sleep_ms(5)

print("Call over;", gateway.stats())
