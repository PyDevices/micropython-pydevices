# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Set what other devices see of the board, and list its pairings.

The board turns its radio on under a name you choose, with a PIN for older
devices that ask for one, and fills its inquiry response: the services it
runs, a web address and data of your own, which a phone or another board
sees when it searches. Then it lists the devices it has paired with, and
again each time one pairs or a pairing is removed. Pairings are kept in
flash, so they survive a reset.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W). btclassic_scan.py on another board shows
the inquiry response.

Run it:

    mpftp run btclassic_settings.py

Then pair your phone or computer with the board. The board prints the new
address.

Tested on: Adafruit QT Py ESP32 Pico, MicroPython 1.30.0-preview with
btclassic, paired with a Windows 11 PC that found its serial port.
"""

import time

import btclassic

# Change these to match your setup.
NAME = "my-board"  # what phones and computers list the board as
PIN = "1234"  # for devices that pair the old way, 1 to 16 characters
URL = "https://micropython.org"  # sent in the inquiry response, or None
MAKER_DATA = b"\xff\xff" + b"hello"  # a company ID (0xFFFF is for tests), then your bytes
FORGET = False  # True forgets every paired device first

# Turn the radio on, then set the name, the PIN and who can find the board.
bt = btclassic.Bluetooth()
bt.active(True)
bt.config(name=NAME, pin=PIN, discoverable=True, connectable=True)

# Start a serial port, so there's a service to list, then fill the inquiry
# response with the services, the web address and the data.
port = btclassic.Serial(bt)
bt.config(eir_uuids=True, eir_url=URL, eir_manufacturer=MAKER_DATA)

# Read the settings back. The address is there only while the radio is on.
print("Name:", bt.config("name"))
print("Address:", bt.config("mac").hex(":"))
print("Class of device: 0x%06x" % bt.config("device_class"))

# Forget every pairing, if asked to.
if FORGET:
    bt.unpair()

# List the paired devices now, and again whenever that list changes.
while True:
    print("Paired with %d device(s):" % len(bt.paired()))
    for address in bt.paired():
        print("  ", address.hex(":"))
    while not bt.changes() & btclassic.PAIRED:
        time.sleep_ms(200)
