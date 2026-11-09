# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Watch a Bluetooth link's signal, and keep out of Wi-Fi's way.

The board serves a serial port over Bluetooth. Before it starts, it stops
hopping on the channels a Wi-Fi network uses, and shortens how long it
calls a device that's away. While a device is connected, it prints the
link's signal once a second, so you can walk the device away and back.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a computer, phone or another board
to connect to its serial port (btclassic_serial_client.py does).

Run it:

    mpftp run btclassic_signal.py

Tested on: Adafruit QT Py ESP32 Pico, MicroPython 1.30.0-preview with
btclassic, with a Windows 11 PC connected to its serial port.
"""

import time

import btclassic

# Change these to match your setup.
WIFI_MHZ = 2437  # the centre of your Wi-Fi channel; 2437 is channel 6
PAGE_TIMEOUT_MS = 3000  # how long connect() calls a device that's away

# Allow every Bluetooth channel (2402 + n MHz) more than 11 MHz from the
# Wi-Fi channel's centre: a bit for each, channel 0 in bit 0.
channels = bytearray(10)
for channel in range(79):
    if abs(2402 + channel - WIFI_MHZ) > 11:
        channels[channel // 8] |= 1 << (channel % 8)

# Set both before the link comes up, then start the serial port.
bt = btclassic.Bluetooth()
bt.active(True)
bt.config(channel_map=channels, page_timeout=PAGE_TIMEOUT_MS)
print("Hopping on %d of 79 channels" % sum(bin(byte).count("1") for byte in channels))
port = btclassic.Serial(bt)
print("Connect to %s's serial port." % bt.config("name"))

while True:
    if port.changes() & btclassic.CONNECTION:
        print("Connected" if port.connected else "Disconnected")

    # The signal is in dB from the range the radio wants: 0 is inside it,
    # negative is weaker.
    if port.connected:
        try:
            print("Signal: %+d dB" % bt.rssi(port.peer))
        except OSError:
            pass
    time.sleep(1)
