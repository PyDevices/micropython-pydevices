# A MIDI effect, not an audio effect: harmonize whatever this board is
# handed. Every note that arrives on the USB MIDI function goes back out
# with a major third and a perfect fifth stacked on top -- melody in,
# triads out. Everything that is not a note (controllers, pitch bend,
# program changes, clock) passes through untouched, which is what being a
# well-mannered MIDI effect means.
#
# The example neither knows nor cares who is on the other end. Tonight
# that is a DAW talking both directions down one cable (USB MIDI is full
# duplex); the day this board grows a second USB voice, the same loop
# ships its output to a hardware synth instead by pointing `send` at the
# other port. The music logic would not change.
#
# Deploy standalone by copying to /main.py: it starts at boot, needs no
# console, and runs until reset -- the pattern for a board that lives its
# life as an instrument. Interactively: `mpftp run examples/midi_harmonizer.py`
# (detached) and interrupt to stop.

import time

import adafruit_midi
from adafruit_midi.note_off import NoteOff
from adafruit_midi.note_on import NoteOn
from midiports import usb_device

# The chord stack, in semitones above each played note. Change these and
# the board becomes a different instrument: (12,) octaves, (3, 7) minor,
# (4, 7, 10) dominant sevenths, (7,) power chords.
HARMONY = (4, 7)

# The board's own USB MIDI function, as adafruit_midi's two ports. The same
# loop drives a hosted controller by opening other ports (midiports.uart for
# a 5-pin cable, midiports.usbhost_shim for a keyboard on the board's USB host).
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
            else:
                midi.send(msg, channel=getattr(msg, "channel", None) or 0)


# The file is its own entry point: copied to /main.py it starts at boot,
# which is what the header promises and what nothing here used to do.
run()
