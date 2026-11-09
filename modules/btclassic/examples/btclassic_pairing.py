# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Pair with a number check, then echo a serial link.

The board serves a serial port over Bluetooth and pairs with a check, as a
phone does: when another device pairs, both show the same six-digit number,
and you answer y at the board's REPL if they match. Once paired, every line
the other device sends comes back in upper case.

Needs: a classic ESP32 board with firmware built with the btclassic module,
and a device to pair from that shows a number: a computer, a phone's serial
terminal app, or another board.

Run it:

    mpftp run --follow btclassic_pairing.py

Tested on: Adafruit QT Py ESP32 Pico with the Audio BFF, MicroPython
1.30.0-preview, paired from an ESP32 Audio Kit.
"""

import select

import btclassic

# Start the radio, say it can show a number and answer yes or no, then
# start the serial port.
bt = btclassic.Bluetooth()
bt.active(True)
bt.config(io=btclassic.IO_DISPLAY_YESNO)
port = btclassic.Serial(bt)
print("Pair with %s." % bt.config("name"))

# Wake for bytes, or every tenth of a second to look for a pairing.
poller = select.poll()
poller.register(port, select.POLLIN)
while True:
    poller.poll(100)

    # Answer a pairing: check the number, or show one for the other side.
    if bt.changes() & btclassic.PASSKEY:
        step = bt.passkey()
        if step:
            address, action, number = step
            if action == btclassic.PASSKEY_COMPARE:
                answer = input("%s shows %06d? [y/n] " % (address.hex(":"), number))
                bt.passkey_reply(answer.strip() == "y")
            elif action == btclassic.PASSKEY_DISPLAY:
                print("Type %06d on %s." % (number, address.hex(":")))

    # Say when a device connects or leaves.
    if port.changes() & btclassic.CONNECTION:
        print("Connected" if port.connected else "Disconnected")

    # Send each line back in upper case.
    line = port.readline()
    if line:
        port.write(line.upper())
