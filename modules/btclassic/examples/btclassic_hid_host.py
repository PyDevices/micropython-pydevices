# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read a Bluetooth keyboard, mouse or gamepad.

The board looks for a keyboard, mouse or game controller in pairing mode,
connects to it and prints what it sends: the text typed, the mouse's moves
and buttons, media keys, and the gamepad's buttons and sticks. Once
connected it turns the keyboard's Num Lock light on.

Needs: a board with firmware built with the btclassic module (a classic
ESP32, or a Pico W or Pico 2 W), and a Bluetooth keyboard, mouse or
gamepad in pairing mode. Another board running btclassic_hid_keyboard.py or
btclassic_hid_gamepad.py works as the device.

Run it:

    mpftp run btclassic_hid_host.py

Tested on: ESP32 Audio Kit, MicroPython 1.30.0-preview with btclassic,
reading an Adafruit QT Py ESP32 Pico running btclassic_hid_gamepad.py.
"""

import time

import btclassic
import network

# Change these to match your setup.
ADDRESS = None  # the device's 6-byte address, or None to look for one
FEATURE_ID = 5  # a feature report to read and change, or None

# Wi-Fi and Bluetooth share the radio. Turn Wi-Fi off for a steadier link.
network.WLAN(network.STA_IF).active(False)
bt = btclassic.Bluetooth()
host = btclassic.HIDHost(bt)

# Look for a device in pairing mode. Major class 5 is peripheral, which
# keyboards, mice and game controllers report.
address = ADDRESS
if address is None:
    print("Looking for a keyboard, mouse or gamepad in pairing mode...")
    bt.inquiry(10240)
    while bt.inquiring() and address is None:
        time.sleep_ms(200)
        for addr, name, cod, rssi in bt.found():
            if (cod >> 8) & 0x1F == 5:
                print("Found", name or addr.hex(":"))
                address = addr
                break
    bt.inquiry(0)
if address is None:
    print("No device found.")
    raise SystemExit

# Connect. The device reconnects by itself after this, when it wakes.
host.connect(address)

held = ()
while True:
    # When the link comes up, say which product it is, and light Num Lock.
    # A keyboard's output report 1 carries its lights, bit 0 Num Lock.
    if host.changes() & btclassic.CONNECTION:
        print("Connected" if host.connected else "Disconnected")
        if host.connected:
            print("Device IDs:", host.ids)
            try:
                host.send(bytes((1, 1)))
            except OSError:
                pass

            # Read a setting the device keeps as a feature report, and add
            # one to it. A device without that report refuses.
            try:
                if FEATURE_ID is not None:
                    value = host.get_report(FEATURE_ID)[1]
                    host.set_report(bytes((FEATURE_ID, value + 1 & 0xFF)))
                    print("Feature report %d: %d, now %d" % (FEATURE_ID, value, value + 1 & 0xFF))
            except OSError:
                pass

    # Take each input report apart and print what it means.
    report = host.read()
    while report:
        event = host.decode(report)
        if event is None:
            print("Report", report.hex())
        elif event[0] == "keyboard":
            # Print each key as it goes down, not again while it's held.
            modifiers, keys = event[1], event[2]
            for key in keys:
                if key not in held:
                    print("Typed", repr(host.char(key, modifiers) or hex(key)))
            held = keys
        elif event[0] != "consumer" or event[1]:
            print(event)
        report = host.read()
    time.sleep_ms(10)
