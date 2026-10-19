# Device MIDI IN: prove the board receives MIDI from a computer.
#
# A sequencer playing *into* the board is the direction a software
# instrument needs most, so audiodsp can render what a DAW sends it. The
# board's own USB MIDI function is midiports' port over micropython-lib's
# usb-device-midi, beside the board's serial port, and adafruit_midi reads
# it. Anything that can send MIDI drives it: a DAW track whose output is
# this board, or a keyboard routed through one.
#
#   mpftp run usbhost/examples/midi_device_in.py -d COM49 --follow --timeout 90
#
# The tally at the end is the evidence: which message types arrived, how
# many of each, and the spread of velocities and channels. A clean run shows
# note-ons with varied velocities (a column of 64s would mean the source
# sends a fixed velocity and the velocity byte is never really tested) and
# note-offs matching them.
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
