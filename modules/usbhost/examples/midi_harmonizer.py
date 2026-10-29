# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Turn every MIDI note the board receives into a chord.

Every note that arrives on the board's USB MIDI function goes back out with a
major third and a perfect fifth stacked on top: melody in, triads out.
Control changes pass through untouched, and other messages are dropped. The
program doesn't care what is on the other end. A DAW talking both directions
down one cable works, because USB MIDI is full duplex, and so would any other
MIDI port you point ``send`` at.
Success is hearing three notes for every one you play.

Needs: a board with USB device support, the ``usb-device-midi`` and
``adafruit_midi`` packages, and a computer or keyboard that sends MIDI to the
board.

Run it, then play into the board's MIDI port:

    mpftp run examples/midi_harmonizer.py

Interrupt to stop. To keep the board as an instrument, copy the file to the
board as ``/main.py``: it then starts at boot, needs no console and runs until
reset.

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-28), plugged into a Windows computer.
"""

import time

import adafruit_midi
from adafruit_midi.control_change import ControlChange
from adafruit_midi.note_off import NoteOff
from adafruit_midi.note_on import NoteOn
from midiports import usb_device

# Change these to match your setup.
# The chord stack, in semitones above each played note. Change these and
# the board becomes a different instrument: (12,) octaves, (3, 7) minor,
# (4, 7, 10) dominant sevenths, (7,) power chords.
HARMONY = (4, 7)

# Open the board's own USB MIDI function as adafruit_midi's two ports. The
# same loop drives other MIDI by opening other ports: midiports.uart for a
# 5-pin cable, midiports.usbhost_shim for a keyboard on the board's USB host.
midi_in, midi_out = usb_device.ports()
midi = adafruit_midi.MIDI(midi_in=midi_in, midi_out=midi_out)


def harmonize(msg):
    # The played note first, then its harmonies, clamped to the MIDI
    # ceiling. Offsets are constant, so the note-off for every harmony is
    # regenerated identically from the note-on's math: no bookkeeping, no
    # stuck notes.
    midi.send(msg, channel=msg.channel)
    kind = NoteOn if isinstance(msg, NoteOn) else NoteOff
    for iv in HARMONY:
        h = msg.note + iv
        if h <= 127:
            midi.send(kind(h, msg.velocity), channel=msg.channel)


def run():
    # adafruit_midi owns the wire rules; midiports hands it whole messages.
    # Poll any() first so a quiet keyboard allocates nothing.
    while True:
        if not midi_in.any():
            time.sleep_ms(2)
            continue
        while True:
            msg = midi.receive()
            if msg is None:
                break
            if isinstance(msg, (NoteOn, NoteOff)):
                harmonize(msg)
            elif isinstance(msg, ControlChange):
                midi.send(msg, channel=msg.channel)


# Start straight away, so the file works as the board's /main.py.
run()
