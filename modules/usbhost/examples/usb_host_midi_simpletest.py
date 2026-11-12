# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a chord on a USB MIDI instrument and print what it sends back.

The board is the USB host and the instrument its device: a keyboard, a drum
machine, or an Android phone in its USB MIDI mode. ``midiports.usb_host`` opens
it as a pair of ports, a MIDI input and output, the same shape as any other
MIDI transport. ``midi_host.py`` parses the messages and names each one.

Needs: a board with USB host support (firmware built with usbhost), the
``midiports`` package, and a MIDI instrument in the host port. For an Android
phone, pick MIDI in its USB notification and open a pad or keyboard app.
Behind a hub on an ESP32-P4, set ``FULL_SPEED = True`` for a keyboard: the P4's
high-speed port can't reach a full-speed device through a hub. A phone needs
no change.

Run it:

    mpftp run --follow examples/usb_host_midi_simpletest.py

Then play. The program sends a chord and prints the bytes of each message the
instrument sends.

Tested on: Waveshare ESP32-P4-WIFI6-DEV-KIT on MicroPython
v1.30.0-preview.160.gb70ae1aca6 (built 2026-11-11), behind its onboard hub, hosting a Samsung Galaxy S21 in MIDI mode running
MIDIPadidi: the chord went out and three pad taps arrived as note-on and
note-off pairs.
"""

import time

import usb.core
from midiports import usb_host

# Change these to match your setup.
FULL_SPEED = False
SECONDS = 20  # how long to listen

# Start the host and wait for the instrument to enumerate.
usb.core.start(full_speed=FULL_SPEED, classes=("midi",))
try:
    device = None
    while device is None:
        time.sleep_ms(250)
        device = usb.core.find(custom_match=usb_host.is_midi)
    midi_in, midi_out = usb_host.ports(device)
    print("Instrument found:", device.product)

    # Send a chord, then release it.
    if midi_out is not None:
        midi_out.write(bytes((0x90, 60, 100, 0x90, 67, 100)))
        time.sleep_ms(500)
        midi_out.write(bytes((0x80, 60, 0, 0x80, 67, 0)))

    # Print the bytes of everything the instrument sends.
    buffer = bytearray(64)
    deadline = time.ticks_add(time.ticks_ms(), SECONDS * 1000)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        count = midi_in.readinto(buffer)
        if count:
            print("Received", bytes(buffer[:count]).hex())
        else:
            time.sleep_ms(5)
    midi_in.close()
finally:
    # Release the port for the next program.
    usb.core.stop()
