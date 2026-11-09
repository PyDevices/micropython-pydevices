# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Send a file to a phone over Bluetooth, and take the files it sends.

The board sends a short text file to a paired phone or computer with
Object Push (OPP), the way a phone shares a photo with another. The phone
asks you to accept it. Then the board listens: share a file or some text
to the board from the phone's share menu, and the board prints its name,
type and size, and saves it.

Needs: a classic ESP32, Pico W or Pico 2 W with firmware built with the
btclassic module, and a phone or computer paired with the board.

Run it:

    mpftp run btclassic_object_push.py

Tested on: Pimoroni Pico Plus 2 W, MicroPython 1.30.0-preview with
btclassic, with a Galaxy S21.
"""

import time

import btclassic

# Change these to match your setup.
ADDRESS = None  # the phone's 6-byte address, or None for the last device
NAME = "hello.txt"  # the file the phone receives
TEXT = "Hello from MicroPython over Bluetooth.\n"

bt = btclassic.Bluetooth()
push = btclassic.ObjectPush(bt)

# Send the file. This waits while the phone asks its user to accept it.
print("Sending %s; accept it on the phone." % NAME)
try:
    push.send(NAME, TEXT.encode(), addr=ADDRESS, type="text/plain")
    print("Sent %d bytes" % push.sent)
except OSError as error:
    print("Not sent:", error)

# Offer Object Push, so the phone's share menu lists the board.
push.listen()
print("Share a file to %s from the phone." % bt.config("name"))
while True:
    if push.changes() & btclassic.PUSH:
        item = push.received()
        if item:
            # Save what came, under its own name, and let the next one in.
            name, kind, data = item
            print("Received %s (%s), %d bytes%s" % (name, kind, len(data), ", cut short" if push.truncated else ""))
            with open(name.split("/")[-1] or "received", "wb") as f:
                f.write(data)
    time.sleep_ms(100)
