# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Type a line of text as a Bluetooth keyboard.

The board becomes a Bluetooth keyboard and mouse. Pair your computer with
the board, click into a text editor, and the board types a line each time
it connects, then nudges the mouse pointer in a small square. After the
first pairing it calls your computer back when it starts. It prints the
keyboard LEDs (Caps Lock and Num Lock) whenever your computer changes them.

Needs: a classic ESP32 board with firmware built with the btclassic module.

Run it:

    mpftp run btclassic_hid_keyboard.py

Tested on: Adafruit QT Py ESP32 Pico, MicroPython v1.30.0-preview with
btclassic, paired with a Windows 11 PC typing into Notepad.
"""

import time

import btclassic
import network

# Change these to match your setup.
TEXT = "Hello from a MicroPython keyboard.\n"
STEP = 40  # mouse move per side of the square, in pointer units

# Wi-Fi and Bluetooth share the ESP32's radio. Turn Wi-Fi off for a
# steadier link.
network.WLAN(network.STA_IF).active(False)

# Start a keyboard with a mouse. The board is listed under its hostname.
bt = btclassic.Bluetooth()
hid = btclassic.HIDDevice(bt, btclassic.HIDDevice.COMBO)
print("Pair with %s and click into a text editor." % network.hostname())

# Call back the computer it last typed into, if there is one.
try:
    hid.connect()
except OSError:
    pass

leds = 0
while True:
    changed = hid.changes()

    # When a computer connects, give it a moment, then type and move.
    if changed & btclassic.CONNECTION:
        print("Connected" if hid.connected else "Disconnected")
        if hid.connected:
            time.sleep(1)
            print("Typed %d characters" % hid.type(TEXT))
            for x, y in ((STEP, 0), (0, STEP), (-STEP, 0), (0, -STEP)):
                hid.mouse(x, y)
                time.sleep_ms(200)

    # The computer sends the keyboard LEDs back as an output report.
    if changed & btclassic.HID and hid.leds != leds:
        leds = hid.leds
        print("LEDs: Num Lock %d, Caps Lock %d" % (leds & 1, leds >> 1 & 1))

    time.sleep_ms(50)
