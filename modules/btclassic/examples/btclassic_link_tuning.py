# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Tune a Bluetooth link's packet types and poll interval.

The board serves a serial port over Bluetooth. When a device connects, the
program asks the controller to use only short packets and to poll the
device every few milliseconds, then prints what the controller says is in
force. Short packets cope better with a noisy band, and a short poll
interval keeps a link responsive.

Needs: a classic ESP32 board with firmware built with the btclassic module,
and a computer, phone or another board to connect to its serial port
(btclassic_serial_client.py does).

Run it:

    mpftp run btclassic_link_tuning.py

Tested on: Ai-Thinker ESP32 Audio Kit, MicroPython 1.30.0-preview with
btclassic, with a Windows 11 PC connected to its serial port.
"""

import time

import btclassic

# Change these to match your setup.
PACKET_TYPES = btclassic.PKT_DM1 | btclassic.PKT_DH1 | btclassic.PKT_DM3  # 1- and 3-slot only
POLL_MS = 10  # the longest the master waits before sending to the slave
MIN_KEY_SIZE = 16  # refuse links that encrypt with a shorter key, in bytes

# The key size applies to every link, so set it before the radio starts.
bt = btclassic.Bluetooth()
bt.config(min_key_size=MIN_KEY_SIZE)
bt.active(True)
port = btclassic.Serial(bt)
print("Connect to %s's serial port." % bt.config("name"))

while True:
    if port.changes() & btclassic.CONNECTION:
        if port.connected:
            # Link settings belong to one connected device.
            try:
                print(bt.tune(port.peer, packet_types=PACKET_TYPES, poll_ms=POLL_MS))
            except OSError as error:
                print("The controller kept its settings:", error)
        else:
            print("Disconnected")
    time.sleep_ms(200)
