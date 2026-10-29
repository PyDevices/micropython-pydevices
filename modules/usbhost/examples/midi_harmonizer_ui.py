# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Choose the harmony from the touchscreen while MIDI notes flow.

The same MIDI effect as ``midi_harmonizer.py``, which takes whatever MIDI it
is handed, adds harmony and sends it back out. Here the chord and the
direction are live-editable from two rows of buttons:

    chord:      Major | Minor | 7th | Power | Octave
    direction:  Up | Down | Both

Changing either sends All Notes Off first, so no harmony outlives the setting
that created it. The MIDI pump rides an LVGL timer inside the display loop.

Needs: a board with a touchscreen and USB device support, a firmware with
LVGL, a ``display_driver`` module that sets up the display and touch, and
the ``usb-device-midi`` and ``adafruit_midi`` packages.

Run it, then play into the board's MIDI port:

    mpftp run examples/midi_harmonizer_ui.py

To keep it as the board's instrument, copy the file to the board as
``/main.py``.
"""

import display_driver  # noqa: F401 -- wires LVGL flush + input + event_loop
import lvgl as lv

import adafruit_midi
from adafruit_midi.note_off import NoteOff
from adafruit_midi.note_on import NoteOn
from midiports import usb_device

CHORDS = (
    ("Major", (4, 7)),
    ("Minor", (3, 7)),
    ("7th", (4, 7, 10)),
    ("Power", (7,)),
    ("Octave", (12,)),
)
DIRECTIONS = ("Up", "Down", "Both")

_state = {"chord": 0, "direction": 0}
_harmony = ()
_styles = []

# The board's own USB MIDI function: midiports' ports over micropython-lib's
# usb-device-midi, read by adafruit_midi. Other midiports transports (a 5-pin
# UART, a keyboard on the board's USB host) open the same two ports.
_midi_in, _port = usb_device.ports()
_midi = adafruit_midi.MIDI(midi_in=_midi_in, midi_out=_port)


def _rebuild_harmony():
    global _harmony
    base = CHORDS[_state["chord"]][1]
    d = DIRECTIONS[_state["direction"]]
    ivs = []
    if d in ("Up", "Both"):
        ivs.extend(base)
    if d in ("Down", "Both"):
        ivs.extend(-iv for iv in base)
    _harmony = tuple(ivs)
    # No harmony may outlive the setting that created it.
    _port.write(bytes([0xB0, 123, 0]))


_rebuild_harmony()



def _emit(status, note, vel):
    _port.write(bytes([status, note, vel]))
    for iv in _harmony:
        h = note + iv
        if 0 <= h <= 127:
            _port.write(bytes([status, h, vel]))


def _pump(_t):
    # Runs from LVGL's timer: drain everything waiting, harmonize notes, pass
    # the rest through. adafruit_midi keeps its buffer between calls, so a
    # message split across two timer ticks survives.
    if not _midi_in.any():
        return
    while True:
        msg = _midi.receive()
        if msg is None:
            return
        if isinstance(msg, (NoteOn, NoteOff)):
            raw = msg.__bytes__()
            _emit(raw[0], raw[1], raw[2])
        else:
            _port.write(msg.__bytes__())


def _matrix(parent, options, key, y_ofs, status_lbl):
    m = lv.buttonmatrix(parent)
    m.set_map(list(options) + [""])
    m.set_button_ctrl_all(lv.buttonmatrix.CTRL.CHECKABLE)
    m.set_one_checked(True)
    m.set_button_ctrl(0, lv.buttonmatrix.CTRL.CHECKED)
    m.set_size(lv.pct(94), 130)
    m.align(lv.ALIGN.TOP_MID, 0, y_ofs)

    def on_change(_e):
        sel = m.get_selected_button()
        if 0 <= sel < len(options):
            _state[key] = sel
            _rebuild_harmony()
            status_lbl.set_text("intervals: " + (" ".join(
                "%+d" % iv for iv in _harmony) or "(none)"))

    m.add_event_cb(on_change, lv.EVENT.VALUE_CHANGED, None)
    return m


def build_ui():
    inst = display_driver.event_loop.current_instance()
    if inst is not None:
        inst.disable()
    try:
        scr = lv.screen_active()
        title = lv.label(scr)
        title.set_text("MIDI Harmonizer")
        title.align(lv.ALIGN.TOP_MID, 0, 18)

        status = lv.label(scr)
        status.align(lv.ALIGN.BOTTOM_MID, 0, -24)
        status.set_text("intervals: +4 +7")

        _matrix(scr, tuple(c[0] for c in CHORDS), "chord", 70, status)
        _matrix(scr, DIRECTIONS, "direction", 230, status)

        lv.timer_create(_pump, 5, None)
    finally:
        if inst is not None:
            inst.enable()


build_ui()
print("harmonizer ui up")
