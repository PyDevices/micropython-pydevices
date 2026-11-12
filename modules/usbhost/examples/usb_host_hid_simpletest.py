# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Print the keys pressed on a USB keyboard plugged into the board.

The board is the USB host and the keyboard its device. ``usb.host.hid`` opens
the keyboard in boot protocol, which every keyboard supports, and returns each
key press and release as an event. The program prints the key's HID usage
code and whether it went down or up, until the time is up. ``hid_host.py``
turns the same events into ``events.Key`` records and lights the lock keys.

Needs: a board with USB host support (firmware built with usbhost), the
``usb-host-hid`` package, and a keyboard in the host port. A board running
``usb_device_hid_simpletest.py`` works as one. Behind a hub on an ESP32-P4,
leave ``FULL_SPEED = True``: a high-speed hub can't reach a full-speed
keyboard otherwise.

Run it:

    mpftp run --follow examples/usb_host_hid_simpletest.py

Then type on the keyboard. Success is a line for each key going down and
coming up.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, with FULL_SPEED = True, hosting an Arduino GIGA R1 WiFi presenting a keyboard
(``usb.device.keyboard``): the H and I keys it sent printed as down and up lines.
"""

import time

import usb.core
from usb.host.hid import KEY_DOWN, KEYBOARD, Device

# Change these to match your setup.
FULL_SPEED = True
SECONDS = 20  # how long to listen

# Start the host and wait for a keyboard to enumerate.
usb.core.start(full_speed=FULL_SPEED, classes=("hid",))
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=Device.match(KEYBOARD))
    keyboard = Device(device, protocol=KEYBOARD, boot=True)
    print("Keyboard:", device.manufacturer, device.product)

    # Print each key event: its HID usage code, and down or up.
    start = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), start) < SECONDS * 1000:
        event = keyboard.read(50)
        if event:
            print("key 0x%02x" % event[1], "down" if event[0] == KEY_DOWN else "up")
    keyboard.close()
finally:
    # Release the port for the next program.
    usb.core.stop()
