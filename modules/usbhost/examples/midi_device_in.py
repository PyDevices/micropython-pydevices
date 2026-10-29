# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Count the MIDI messages a computer sends to the board.

The board presents a USB MIDI function (midiports' port over micropython-lib's
``usb-device-midi``) beside its serial port, and ``adafruit_midi`` reads it.
Anything that can send MIDI drives it: a DAW track whose output is this
board, or a keyboard routed through one. The tally at the end shows which
message types arrived, how many of each, and the spread of velocities and
channels. A clean run shows note-ons with varied velocities and matching
note-offs. A column of 64s means the source sends a fixed velocity, so the
velocity byte was never really tested.

Needs: a board with USB device support, the ``usb-device-midi`` and
``adafruit_midi`` packages, and a computer that sends MIDI to the board.

Run it, then play into the board's MIDI port:

    mpftp run examples/midi_device_in.py --follow --timeout 90

Tested on: an ESP32-P4 board on MicroPython v1.30.0-preview.160.gb70ae1aca6
(built 2026-10-28), plugged into a Windows computer.
"""

import time

import adafruit_midi
from adafruit_midi.channel_pressure import ChannelPressure
from adafruit_midi.control_change import ControlChange
from adafruit_midi.note_off import NoteOff
from adafruit_midi.note_on import NoteOn
from adafruit_midi.pitch_bend import PitchBend
from adafruit_midi.polyphonic_key_pressure import PolyphonicKeyPressure
from adafruit_midi.program_change import ProgramChange
from midiports import usb_device

# Change these to match your setup.
DURATION_MS = 20000  # how long to listen

NAMES = (
    (NoteOn, "note-on"), (NoteOff, "note-off"), (PolyphonicKeyPressure, "aftertouch"),
    (ControlChange, "cc"), (ProgramChange, "program"), (ChannelPressure, "pressure"),
    (PitchBend, "pitch-bend"),
)


def main():
    midi_in, midi_out = usb_device.ports()
    midi = adafruit_midi.MIDI(midi_in=midi_in, midi_out=midi_out)
    print("listening for {} s: play into this board's MIDI port".format(DURATION_MS // 1000))
    counts = {}
    velocities = set()
    channels = set()
    end = time.ticks_add(time.ticks_ms(), DURATION_MS)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if not midi_in.any():
            time.sleep_ms(2)
            continue
        while True:
            msg = midi.receive()
            if msg is None:
                break
            name = next((n for cls, n in NAMES if isinstance(msg, cls)), type(msg).__name__)
            counts[name] = counts.get(name, 0) + 1
            if isinstance(msg, NoteOn) and msg.velocity:
                velocities.add(msg.velocity)
            if getattr(msg, "channel", None) is not None:
                channels.add(msg.channel + 1)
    print("received:", counts or "nothing")
    print("velocities seen:", len(velocities), sorted(velocities)[:12])
    print("channels:", sorted(channels))
    return bool(counts)


if __name__ == "__main__":
    main()
