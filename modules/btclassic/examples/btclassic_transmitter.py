# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Send the board's line in to Bluetooth headphones or a speaker.

The board records its audio input and streams it to a receiver over A2DP,
as a Bluetooth transmitter on an old stereo or a TV does. It looks for
nearby audio devices and connects to the first one, or to the address you
set, then prints the receiver's keys and the stream's counters.

Needs: a classic ESP32 board, firmware built with the btclassic module, a
board config with a pcm_in role (the ESP32 Audio Kit records its line-in
jack), and Bluetooth headphones or a speaker in pairing mode.

Run it:

    mpftp run btclassic_transmitter.py

Plug a music player into the line-in jack. Its sound plays on the
receiver about half a second later.

Tested on: ESP32 Audio Kit (ESP32-A1S), MicroPython 1.30.0-preview,
playing to a Windows 11 PC as the receiver.
"""

import time

import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

# Change these to match your setup.
ADDRESS = None  # the receiver's 6-byte address, or None to look for one
RATE = 44100  # Hz; the stream's own rate, so nothing is converted
FRAMES = 512  # frames per write; 512 is about 12 ms

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

# Open the input. A board that can choose records its line-in jack.
fmt = AudioFormat(RATE, 2, 16)
line = getattr(board_peripherals, "LINE", None)
if line:
    recorder = board_peripherals.pcm_in(fmt, input=line)
else:
    recorder = board_peripherals.pcm_in(fmt)
recorder.open()

# Start the source and connect. The stream starts once the link is up.
source = btclassic.AudioSource(bt, rate=RATE, channels=2)
source.connect(address)
source.track("Line in", "Bluetooth transmitter")
source.status = "playing"

buf = bytearray(FRAMES * 4)
last = time.ticks_ms()
while True:
    # Move one block from the input to the receiver. While the stream runs,
    # write() waits for room, so the receiver sets the pace.
    recorder.readinto(buf)
    source.write(buf)

    # Report the link as it changes, and each key pressed on the receiver.
    if source.changes() & btclassic.CONNECTION:
        if source.connected:
            print("Connected to", source.peer_name or "the receiver")
        else:
            print("Not connected")
    key = source.key()
    while key:
        if key[1]:
            print("Key", hex(key[0]))
        key = source.key()

    # Print the counters every five seconds.
    if time.ticks_diff(time.ticks_ms(), last) >= 5000:
        last = time.ticks_ms()
        print("source", source.stats(), "input", recorder.stats())
