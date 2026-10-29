# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read a USB MIDI instrument on the host port, and play notes back to it.

The board becomes the USB host and a commercial MIDI device plugs into it: a
keyboard, a drum machine, a sound module. No computer is in the chain. This
is the other half of ``midi_harmonizer.py``, where the board is a MIDI
device driven by a host; here the roles are reversed. Both directions use
the same plain MIDI byte stream, so code that parses or generates MIDI
doesn't care which end it is on, and the USB-MIDI 32-bit packet framing never
reaches you. The ports come from ``midiports.usb_host``: a hosted MIDI device
as a ``(midi_in, midi_out)`` pair on ``usb.core``, the same pair
``adafruit_midi`` reads and writes on any other MIDI transport.

An Android phone works too. Pull down the USB notification and choose MIDI
(on Samsung, "USB controlled by" > "MIDI"). A pad or keyboard app on the
phone, such as MIDIPadidi, then plays into the board like any controller. A
phone is a high-speed device, so it needs no ``FULL_SPEED`` behind a hub.

Needs: a board with USB host support (firmware built with usbhost) and the
``midiports`` package from pydevices, installed with ``mip``. Behind a hub
on an ESP32-P4, set ``FULL_SPEED = True`` below: a keyboard is a full-speed
device, and the P4's high-speed port can't reach one through a hub without a
transaction translator, which the ESP-IDF host lacks. At full speed the hub
passes it straight through.

Run it, then play on the instrument:

    mpftp run examples/midi_host.py
"""

import time

import usb.core
from midiports import message_length, usb_host

# Change these to match your setup.
# True when the device is behind a hub on an ESP32-P4: the host then runs at
# full speed, so the hub passes a full-speed device through. Plugged straight
# into the port, or on an S3, leave it False.
FULL_SPEED = False

# Two-note chord sent to the device, to prove the OUT pipe. Middle C and the
# fifth above it, loud enough to hear on a sound module.
_NOTE_ON = 0x90
_NOTE_OFF = 0x80
_CHORD = (60, 67)


def describe(status, d1, d2):
    """Human-readable line for one MIDI message, for the console."""
    kind = status & 0xF0
    ch = (status & 0x0F) + 1
    if kind == 0x90 and d2:
        return "note on  ch{} note {} vel {}".format(ch, d1, d2)
    if kind == 0x80 or (kind == 0x90 and not d2):
        return "note off ch{} note {}".format(ch, d1)
    if kind == 0xB0:
        return "control  ch{} cc {} = {}".format(ch, d1, d2)
    if kind == 0xE0:
        return "bend     ch{} {}".format(ch, (d2 << 7 | d1) - 8192)
    if kind == 0xC0:
        return "program  ch{} {}".format(ch, d1)
    return "status {:02x} {} {}".format(status, d1, d2)


class Messages:
    """Splits the port's bytes into ``(status, data)`` messages.

    ``midi_in`` hands out whole messages, each starting with its status
    byte, but one read can end partway through one. The bytes so far are
    kept for the next read rather than guessed at. A SysEx is skipped to
    its 0xF7.
    """

    def __init__(self):
        self._msg = []
        self._need = 0
        self._sysex = False

    def feed(self, data):
        out = []
        for b in data:
            if b >= 0xF8:
                out.append((b, b""))         # realtime
            elif self._sysex:
                self._sysex = b != 0xF7
            elif b == 0xF0:
                self._sysex = True
            elif b >= 0x80:
                self._msg = [b]
                self._need = message_length(b)
                if not self._need:
                    out.append((b, b""))
            elif self._msg:
                self._msg.append(b)
                if len(self._msg) == self._need + 1:
                    out.append((self._msg[0], bytes(self._msg[1:])))
                    self._msg = []
        return out


def find_midi(timeout_ms=10000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=usb_host.is_midi)
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def main():
    usb.core.start(full_speed=FULL_SPEED, classes=("midi",))
    dev = find_midi()
    if dev is None:
        print("no MIDI device attached")
        print("stats", usb.core.stats())
        usb.core.stop()
        return

    midi_in, midi_out = usb_host.ports(dev)
    print("device {:04x}:{:04x} {} {}".format(
        dev.idVendor, dev.idProduct, dev.manufacturer, dev.product))
    print("opened -- play something (30 s)")

    # Send a chord first: on a sound module this is audible proof the OUT
    # pipe works before a single key is pressed.
    if midi_out is None:
        # A keyboard with no OUT pipe is a legitimate device, not a failure.
        print("no OUT pipe on this device (receive-only)")
    else:
        n = midi_out.write(bytes([_NOTE_ON, _CHORD[0], 100,
                                  _NOTE_ON, _CHORD[1], 100]))
        print("sent a chord,", n, "bytes accepted")

    buf = bytearray(64)
    messages = Messages()
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < 30000:
        n = midi_in.readinto(buf)
        if n:
            for status, data in messages.feed(buf[:n]):
                if status >= 0xF8:
                    continue                # realtime clock/sense: not worth printing
                d1 = data[0] if data else 0
                d2 = data[1] if len(data) > 1 else 0
                print(" ", describe(status, d1, d2))
        else:
            time.sleep_ms(5)

    if midi_out is not None:
        try:
            midi_out.write(bytes([_NOTE_OFF, _CHORD[0], 0,
                                  _NOTE_OFF, _CHORD[1], 0]))
        except usb.core.USBError:
            pass

    if midi_in.desync:
        print("desync:", midi_in.desync, "byte(s) arrived with no status")
    print("stats", usb.core.stats())
    midi_in.close()
    usb.core.stop()
    print("done")


main()
