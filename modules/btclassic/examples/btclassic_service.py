# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List a serial port under a UUID and a name of your own.

The board serves a Bluetooth serial port and adds a service record for it,
under a UUID you make up and a name. A computer or phone that asks which
services the board offers sees the record, and the channel it points at is
the serial port's. The program prints each connection and echoes what it
reads, in upper case.

Needs: a classic ESP32 board with firmware built with the btclassic module,
and a computer, phone or another board to look the service up
(Bluetooth.services() on another board lists the UUID).

Run it:

    mpftp run btclassic_service.py

Tested on: Ai-Thinker ESP32 Audio Kit, MicroPython 1.30.0-preview with
btclassic, with a Windows 11 PC listing the board's serial services.
"""

import time

import btclassic

# Change these to match your setup. Make your own UUID: any 16 bytes.
SERVICE_UUID = bytes.fromhex("6f3a9c12b4d14e5aa7c10d2e5f8b9a01")
SERVICE_NAME = "Sensor feed"  # 1 to 31 bytes

# The serial port comes first, so its RFCOMM channel is known.
bt = btclassic.Bluetooth()
bt.active(True)
port = btclassic.Serial(bt, name=SERVICE_NAME, timeout=200)

# Add the record that names the service and points at the port's channel.
feed = btclassic.Service(bt, SERVICE_UUID, SERVICE_NAME, channel=port.channel)
print("Serving %r on RFCOMM channel %d" % (SERVICE_NAME, port.channel))

while True:
    if port.changes() & btclassic.CONNECTION:
        print("Connected" if port.connected else "Disconnected")
    data = port.read()
    if data:
        port.write(data.upper())
    else:
        time.sleep_ms(50)
