# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Talk between two boards over an L2CAP channel of your own.

Run it on two boards. The first waits on PSM 0x1001 and sends back every
line in upper case. On the second, set SERVER to the first board's address
(it prints it): that one calls, sends a line each second, and prints what
comes back.

Needs: two boards with firmware built with the btclassic module: classic
ESP32s, Pico Ws or Pico 2 Ws, in any mix.

Run it:

    mpftp run --follow btclassic_l2cap.py

Tested on: an Adafruit QT Py ESP32 Pico and an ESP32 Audio Kit, each in both
roles, MicroPython 1.30.0-preview.
"""

import time

import btclassic

PSM = 0x1001
SERVER = None  # the waiting board's address, such as bytes.fromhex("4c7525eed70a")

bt = btclassic.Bluetooth()
bt.active(True)

if SERVER is None:
    # Wait for a call, then send each line back in upper case.
    ch = btclassic.L2CAP(bt, PSM)
    print("Waiting on PSM 0x%04x at %s." % (PSM, bt.config("mac").hex()))
    while True:
        if ch.changes() & btclassic.CONNECTION:
            print("Connected" if ch.connected else "Disconnected")
        line = ch.readline()
        if line:
            ch.write(line.upper())
        time.sleep_ms(10)
else:
    # Call the waiting board, then send a line a second and print the answer.
    ch = btclassic.L2CAP(bt, timeout=2000)
    ch.connect(SERVER, PSM)
    print("Connected; packets carry up to %d bytes." % ch.mtu)
    for n in range(10):
        ch.write(b"hello %d\n" % n)
        print(ch.readline())
        time.sleep(1)
    ch.deinit()
