# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Call another board's Bluetooth serial port and talk to it.

The board connects to the serial port another device serves (SPP), sends
a numbered line every second and prints each line that comes back. Run
btclassic_serial_echo.py on the other board, and the lines come back in
upper case.

Needs: two boards with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), one running btclassic_serial_echo.py. Set
ADDRESS to that board's address; btclassic_settings.py prints it.

Run it:

    mpftp run btclassic_serial_client.py

Tested on: Pimoroni Pico Plus 2 W on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11) calling an Adafruit QT Py
ESP32 Pico running btclassic_serial_echo.py: ten lines came back in upper case.
"""

import time

import btclassic
import network

# Change these to match your setup.
ADDRESS = "aa:bb:cc:dd:ee:ff"  # the other board's Bluetooth address
LINES = 10  # how many lines to send

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Connect. Channel 0 asks the other board which channel its port is on.
bt = btclassic.Bluetooth()
port = btclassic.Serial(bt, timeout=2000)
port.connect(bytes.fromhex(ADDRESS.replace(":", "")))

# Wait for the link to come up.
for _ in range(100):
    if port.connected:
        break
    time.sleep_ms(100)
if not port.connected:
    print("Couldn't connect to", ADDRESS)
    raise SystemExit
print("Connected to", ADDRESS)

# Send a line, then read the reply; read waits up to the timeout for it.
for number in range(1, LINES + 1):
    port.write("hello %d\n" % number)
    print("Got", port.readline())
    time.sleep(1)

# Print the byte counts and hang up.
print(port.stats())
port.disconnect()
