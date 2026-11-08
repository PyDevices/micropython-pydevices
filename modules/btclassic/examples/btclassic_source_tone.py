# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a tone on Bluetooth headphones or a speaker.

The board looks for nearby audio devices, connects to the first one (or to
the address you set), and plays a steady tone to it. The receiver's display
shows a title and artist, and the board prints each key pressed on the
receiver: play, pause, skip and volume.

Needs: a classic ESP32 board with firmware built with the btclassic module,
and Bluetooth headphones, a speaker or a car stereo in pairing mode.

Run it:

    mpftp run btclassic_source_tone.py

Tested on: Adafruit QT Py ESP32 Pico with the Audio BFF, MicroPython
1.30.0-preview, playing to a Windows 11 PC as the
receiver.
"""

import math
import time

import btclassic
import network

# Change these to match your setup.
ADDRESS = None  # the receiver's 6-byte address, or None to look for one
RATE = 16000  # Hz, the rate the tone is made at
FREQUENCY = 500  # Hz; RATE / FREQUENCY should be a whole number
LEVEL = 8000  # peak, out of 32767

# Wi-Fi and Bluetooth share the ESP32's radio. Turn Wi-Fi off for audio.
network.WLAN(network.STA_IF).active(False)
bt = btclassic.Bluetooth()

# Look for an audio device. Major class 4 is audio and video, which
# headphones, speakers and car stereos report.
address = ADDRESS
if address is None:
    print("Looking for headphones or a speaker in pairing mode...")
    bt.inquiry(10240)
    while bt.inquiring() and address is None:
        time.sleep_ms(200)
        for addr, name, cod, rssi in bt.found():
            if (cod >> 8) & 0x1F == 4:
                print("Found", name or addr.hex(":"), rssi, "dBm")
                address = addr
                break
    bt.inquiry(0)
if address is None:
    raise SystemExit("No audio device found.")

# Start the source, connect, and say what's playing. The stream starts by
# itself once the link is up.
source = btclassic.AudioSource(bt, rate=RATE, channels=1)
source.connect(address)
source.track("Test tone", "btclassic")
source.status = "playing"

# Make one block of whole periods, so blocks join without a click.
period = RATE // FREQUENCY
block = bytearray(period * 20 * 2)
for i in range(period * 20):
    sample = int(LEVEL * math.sin(2 * math.pi * i / period))
    block[2 * i] = sample & 0xFF
    block[2 * i + 1] = (sample >> 8) & 0xFF

receiver = ""
while True:
    # Write the tone. While the stream runs, write() waits for room, so the
    # receiver sets the pace; before then it returns at once.
    source.write(block)
    if not source.streaming:
        time.sleep_ms(20)

    # Report the link as it changes.
    changed = source.changes()
    if changed & btclassic.CONNECTION:
        print("Connected" if source.connected else "Not connected")
    if changed & btclassic.PEER and source.peer_name != receiver:
        receiver = source.peer_name
        if receiver:
            print("The receiver is", receiver)
    if changed & btclassic.FORMAT and source.sample_rate:
        print("Streaming at", source.sample_rate, "Hz")

    # Print each key pressed on the receiver.
    key = source.key()
    while key:
        code, pressed = key
        if pressed:
            print("Key", hex(code))
        key = source.key()
