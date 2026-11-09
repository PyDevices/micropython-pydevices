# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Ask a Bluetooth device what it offers.

The board finds the devices nearby, or takes the address you set, and
asks each one's service directory (SDP) which services it runs: a speaker,
a serial port, a keyboard, a phone's contacts. It prints each with a name
for the common ones. No pairing is needed.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a device nearby that can be found,
or the address of one in range. On the ESP32 the list holds the profiles
its Bluetooth stack knows; a Pico W lists every service.

Run it:

    mpftp run btclassic_services.py

Tested on: ESP32 Audio Kit, MicroPython 1.30.0-preview with btclassic,
asking an Adafruit QT Py ESP32 Pico running btclassic_serial_echo.py.
"""

import time

import btclassic

# Change these to match your setup.
ADDRESS = None  # a device's address, "aa:bb:cc:dd:ee:ff", or None to search

# Names for the service class UUIDs a user meets most.
NAMES = {
    0x1101: "serial port",
    0x1105: "file push",
    0x110A: "sends audio",
    0x110B: "plays audio",
    0x110C: "remote control target",
    0x110E: "remote control",
    0x1112: "headset gateway",
    0x1115: "network",
    0x1116: "network access point",
    0x111E: "hands-free",
    0x111F: "hands-free gateway",
    0x1124: "keyboard, mouse or gamepad",
    0x112F: "phone book",
    0x1132: "messages",
    0x1200: "device ID",
}

bt = btclassic.Bluetooth()
bt.active(True)

# Search for devices, unless an address is set.
if ADDRESS:
    devices = [(bytes.fromhex(ADDRESS.replace(":", "")), ADDRESS)]
else:
    print("Searching...")
    bt.inquiry(10240)
    while bt.inquiring():
        time.sleep_ms(100)
    devices = [(found[0], found[1]) for found in bt.found()]

# Ask each device for its services. A device that has gone away takes the
# page timeout to fail.
for address, name in devices:
    print("%s (%s):" % (address.hex(":"), name or "no name"))
    try:
        for uuid in bt.services(address):
            if isinstance(uuid, int):
                print("   0x%04x %s" % (uuid, NAMES.get(uuid, "")))
            else:
                print("   ", uuid.hex())
    except OSError as error:
        print("    no answer:", error)
