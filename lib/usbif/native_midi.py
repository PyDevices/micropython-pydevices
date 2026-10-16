"""MIDI backend for a board, over the ``_usbif`` native module.

A MIDI device attached to the board acting as host, as a
:class:`usbif.MidiPort`: plain MIDI bytes both ways, the same contract the
``winmm`` and ALSA backends satisfy. Its id is ``"host:<device id>"``.

The board's own USB MIDI function (the board as the device a computer plays)
moved to ``midiports.usb_device``, over micropython-lib's ``usb-device-midi``;
asking here for ``"dev:midi"`` says so.
"""

from . import INOUT, MidiPort, MidiPortInfo

try:
    import _usbif
except ImportError:  # pragma: no cover - exercised only off-target
    _usbif = None

DEVICE_ID = "dev:midi"
_HOST_PREFIX = "host:"


def _require():
    if _usbif is None:
        raise ImportError(
            "the usbif native module is not present in this firmware; "
            "usbif.auto.midi_ports() reports what this platform offers"
        )
    return _usbif


def ports():
    """The board's own MIDI function, plus any MIDI device it is hosting."""
    if _usbif is None:
        return ()
    found = []
    try:
        attached = _usbif.host_devices()
    except (AttributeError, OSError):
        attached = ()
    for dev in attached:
        if "midi" in (dev[5] or ()):
            name = dev[3] or "USB MIDI %04x:%04x" % (dev[1] or 0, dev[2] or 0)
            found.append(MidiPortInfo(id="{}{}".format(_HOST_PREFIX, dev[0]),
                                      name=name, direction=INOUT))
    return tuple(found)


def find(name, direction=None):
    needle = name.lower()
    return tuple(p for p in ports()
                 if needle in (p.name or "").lower()
                 and (direction is None or p.direction in (direction, INOUT)))


def _split_id(port_id):
    text = str(port_id)
    if text == DEVICE_ID:
        return "dev", None
    if text.startswith(_HOST_PREFIX):
        rest = text[len(_HOST_PREFIX):]
        if rest.isdigit():
            return "host", int(rest)
    raise ValueError(
        "bad MIDI port id {!r}; expected {!r} or 'host:<device id>'".format(port_id, DEVICE_ID))


def open_port(port):
    """Open the board's MIDI function, or a hosted MIDI device."""
    _require()
    info = port if isinstance(port, MidiPortInfo) else None
    port_id = info.id if info is not None else port
    role, dev_id = _split_id(port_id)
    if info is None:
        info = MidiPortInfo(id=port_id, direction=INOUT,
                            name="usbif device MIDI" if role == "dev"
                            else "hosted MIDI device {}".format(dev_id))
    if role == "dev":
        raise ValueError("the board's own USB MIDI is midiports.usb_device now, not usbif")
    return NativeHostMidi(info, dev_id)


class NativeHostMidi(MidiPort):
    """A MIDI device attached to the board, with the board as host."""

    def __init__(self, info, dev_id):
        MidiPort.__init__(self, info)
        _usbif.host_midi_open(dev_id)
        self._dev_id = dev_id

    def _read(self, buf):
        return _usbif.host_midi_read(buf)

    def _write(self, data):
        return _usbif.host_midi_write(bytes(data))

    def _close(self):
        _usbif.host_midi_close()

    def dropped(self):
        """``(rx_dropped, release_failed)`` from the host MIDI driver.

        Exposed because a silent drop is the one failure a MIDI stream cannot
        show you: missing notes look like a quiet passage.
        """
        return _usbif.host_midi_dropped()
