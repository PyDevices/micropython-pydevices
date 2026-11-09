# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Be a Bluetooth gamepad that turns its stick in a circle.

The board becomes a game controller with 16 buttons, a hat and two sticks.
Once a computer, phone or another board connects, it turns the left stick
in a slow circle and presses the buttons one after another. It also keeps
a setting the host can read and change, a feature report, and prints the
setting whenever the host changes it.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W). On Windows, the game controller settings
(joy.cpl) show the stick and buttons move; btclassic_hid_host.py on another
board prints them.

Run it:

    mpftp run btclassic_hid_gamepad.py

Tested on: Adafruit QT Py ESP32 Pico, MicroPython 1.30.0-preview with
btclassic, read by a Windows 11 PC's game controller settings.
"""

import math
import time

import btclassic
import network

# Change these to match your setup.
RADIUS = 100  # how far the stick turns, up to 127
SETTING_ID = 5  # the feature report's ID
SETTING = 3  # its starting value, 0 to 255

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)

# Start a gamepad. Pair with it from the host; it is listed under the
# board's hostname.
bt = btclassic.Bluetooth()
pad = btclassic.HIDDevice(bt, btclassic.HIDDevice.GAMEPAD)
print("Pair with %s from your computer, phone or another board." % network.hostname())

# Publish a setting the host can read and change, report ID first.
pad.set_feature(bytes((SETTING_ID, SETTING)))

# Call back the host it last connected to, if there is one.
try:
    pad.connect()
except OSError:
    pass

setting = SETTING
step = 0
while True:
    changed = pad.changes()
    if changed & btclassic.CONNECTION:
        print("Connected" if pad.connected else "Disconnected")

    # A host that writes the feature report raises HID; read the new value.
    if changed & btclassic.HID:
        report = pad.feature(SETTING_ID)
        if report and report[1] != setting:
            setting = report[1]
            print("The host set the setting to", setting)

    # Turn the stick one step round the circle, and hold one button down
    # at a time, moving on every 20 steps.
    if pad.connected:
        angle = 2 * math.pi * step / 100
        x = int(RADIUS * math.cos(angle))
        y = int(RADIUS * math.sin(angle))
        button = 1 << (step // 20 % 16)
        pad.gamepad(button, x=x, y=y)
        if step % 20 == 0:
            print("Button", step // 20 % 16 + 1, "stick", x, y)
        step += 1
    time.sleep_ms(50)
