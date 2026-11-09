# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List the Bluetooth devices nearby and what each one says about itself.

The board runs an inquiry, then prints each device it found: its address,
name, Class of Device and signal, and the fields of its extended inquiry
response, such as the services it offers and any data of its own.

Needs: a classic ESP32 board with firmware built with the btclassic module,
and a device nearby that can be found: a phone on its Bluetooth settings
page, or headphones in pairing mode.

Run it:

    mpftp run btclassic_scan.py

Tested on: Adafruit QT Py ESP32 Pico with the Audio BFF, MicroPython
1.30.0-preview, finding an ESP32 Audio Kit.
"""

import time

import btclassic

# Change these to match your setup.
SECONDS = 10.24  # how long to look, in steps of 1.28 s

# The inquiry response's field types this example names.
FIELDS = {
    0x02: "UUIDs (some)",
    0x03: "UUIDs",
    0x08: "short name",
    0x09: "name",
    0x0A: "TX power",
    0x24: "URI",
    0xFF: "maker data",
}

# Look for devices, and wait for the inquiry to end.
bt = btclassic.Bluetooth()
bt.active(True)
bt.inquiry(int(SECONDS * 1000))
while bt.inquiring():
    time.sleep_ms(100)

for address, name, cod, rssi in bt.found():
    print("%s %r class %06x rssi %s" % (address.hex(":"), name, cod, rssi))

    # Walk the response's fields: a length byte, a type byte, then data.
    eir = bt.eir(address) or b""
    i = 0
    while i < len(eir) and eir[i]:
        kind, data = eir[i + 1], eir[i + 2 : i + 1 + eir[i]]
        if kind in (0x02, 0x03):
            # 16-bit service UUIDs, little-endian.
            data = " ".join("%04x" % (data[j] | data[j + 1] << 8) for j in range(0, len(data) - 1, 2))
        print("    %s: %s" % (FIELDS.get(kind, hex(kind)), data))
        i += 1 + eir[i]
