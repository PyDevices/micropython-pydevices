# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Echo what a Bluetooth serial terminal sends, in upper case.

The board serves a serial port over Bluetooth. Pair your computer with the
board and open the Bluetooth COM port it adds, or connect a phone's serial
terminal app: every line you send comes back in upper case.

Needs: a classic ESP32 board with firmware built with the btclassic module.

Run it:

    mpftp run btclassic_serial_echo.py

Tested on: not yet run.
"""

import select

import btclassic
import network

# Wi-Fi and Bluetooth share the ESP32's radio. Turn Wi-Fi off for a
# steadier link.
network.WLAN(network.STA_IF).active(False)

# Start the serial port. The board is listed under its hostname.
bt = btclassic.Bluetooth()
port = btclassic.Serial(bt)
print("Connect to %s's serial port." % network.hostname())

# Wait for bytes with poll, as with any stream.
poller = select.poll()
poller.register(port, select.POLLIN)
while True:
    poller.poll(1000)

    # Say when a device connects or leaves.
    if port.changes() & btclassic.CONNECTION:
        print("Connected" if port.connected else "Disconnected")

    # Send each line back in upper case.
    line = port.readline()
    if line:
        port.write(line.upper())
