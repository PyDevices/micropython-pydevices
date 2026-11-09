# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Send a file to a phone, then take one it sends back.

The board sends a short text file to a paired phone or computer over Object
Push, which asks its user to accept it. Then it listens: share a file from
the phone to the board, and the board prints its name, type and size, and
the start of what it holds.

Needs: a classic ESP32 or a Pico W with firmware built with the btclassic
module, and a phone or computer paired with the board.

Run it:

    mpftp run --follow btclassic_object_push.py

Tested on: not yet run.
"""

import time

import btclassic

# Change these to match your setup.
PEER = None  # the phone's address, such as bytes.fromhex("a0b1c2d3e4f5"); None for the first paired
NAME = "hello.txt"
WAIT = 120  # seconds to wait for a file from the phone

bt = btclassic.Bluetooth()
bt.active(True)
opp = btclassic.ObjectPush(bt)

# Send the file. send() waits until the phone takes it or turns it away.
try:
    opp.send(NAME, b"Hello from the board.\n", addr=PEER, type="text/plain")
    print("Sent %s (%d bytes)." % (NAME, opp.sent))
except OSError as e:
    print("Not sent:", e)

# Offer Object Push, and wait for the phone to share a file with the board.
opp.listen()
print("Listening: share a file to the board from the phone.")
deadline = time.time() + WAIT
got = None
while got is None and time.time() < deadline:
    got = opp.received()
    time.sleep_ms(100)

if got:
    name, mime, data = got
    print("Received %s (%s), %d bytes:" % (name, mime, len(data)))
    print(data[:200])
else:
    print("Nothing came in %d seconds." % WAIT)
opp.deinit()
