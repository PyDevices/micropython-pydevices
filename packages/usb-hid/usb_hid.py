# MicroPython usb_hid: CircuitPython's usb_hid module, over micropython-lib's
# usb-device-hid.
# MIT license; Copyright (c) 2026 Brad Barnett
#
# Adafruit's adafruit_hid drives these devices exactly as it drives
# CircuitPython's:
#
#     import usb_hid
#     from adafruit_hid.keyboard import Keyboard
#
#     usb_hid.enable()                       # keyboard, mouse, consumer control
#     kb = Keyboard(usb_hid.devices)
#     kb.send(Keycode.A)
#
# enable() starts the USB device with the board's own serial port kept, as
# CircuitPython's boot.py does. To put HID beside other functions (MIDI, a
# drive), make the interface and start them together:
#
#     itf = usb_hid.interface((usb_hid.Device.KEYBOARD, usb_hid.Device.MOUSE))
#     usb.device.get().init(itf, midi_itf, builtin_driver=True)
#
# All the devices share one HID interface, told apart by report ID, as on
# CircuitPython. The keyboard's LED report arrives through SET_REPORT and is
# kept for get_last_received_report().
import time

from micropython import const
from usb.device.hid import HIDInterface

_EP_BUSY_WAIT_MS = const(100)

# CircuitPython's own descriptors (shared-module/usb_hid/report_descriptors.c),
# byte for byte, so adafruit_hid's reports fit them.
_KEYBOARD = bytes.fromhex(
    "05010906a1018501050719e029e71500250175019508810295017508810195057501050819012905910295017503910195067508150026ff00050719002aff008100c0"
)
_MOUSE = bytes.fromhex(
    "05010902a1010901a1008502050919012905150025019505750181029501750381010501093009311581257f75089502810609381581257f750895018106c0c0"
)
_CONSUMER = bytes.fromhex("050c0901a1018503751095011501268c0219012a8c028100c0")


class Device:
    """One HID device, in CircuitPython's ``usb_hid.Device`` shape."""

    def __init__(
        self,
        *,
        report_descriptor,
        usage_page,
        usage,
        report_ids,
        in_report_lengths,
        out_report_lengths,
    ):
        self.report_descriptor = bytes(report_descriptor)
        self.usage_page = usage_page
        self.usage = usage
        self.report_ids = tuple(report_ids)
        self.in_report_lengths = tuple(in_report_lengths)
        self.out_report_lengths = tuple(out_report_lengths)
        self._interface = None
        self._received = {}

    def _index(self, report_id):
        if report_id is None:
            if len(self.report_ids) != 1:
                raise ValueError("report_id needed: this device has several")
            return 0
        try:
            return self.report_ids.index(report_id)
        except ValueError:
            raise ValueError("report_id {} is not this device's".format(report_id))

    def send_report(self, report, report_id=None):
        """Send one input report. Waits for the host to take the last one."""
        i = self._index(report_id)
        if len(report) != self.in_report_lengths[i]:
            raise ValueError("report is {} bytes, not {}".format(len(report), self.in_report_lengths[i]))
        itf = self._interface
        if itf is None or not itf.is_open():
            raise OSError("USB HID is not connected to a host")
        itf._send(self.report_ids[i], report)

    def get_last_received_report(self, report_id=None):
        """The last output report the host sent (a keyboard's LEDs), or None."""
        return self._received.get(self.report_ids[self._index(report_id)])


Device.KEYBOARD = Device(
    report_descriptor=_KEYBOARD,
    usage_page=0x01,
    usage=0x06,
    report_ids=(1,),
    in_report_lengths=(8,),
    out_report_lengths=(1,),
)
Device.MOUSE = Device(
    report_descriptor=_MOUSE,
    usage_page=0x01,
    usage=0x02,
    report_ids=(2,),
    in_report_lengths=(4,),
    out_report_lengths=(0,),
)
Device.CONSUMER_CONTROL = Device(
    report_descriptor=_CONSUMER,
    usage_page=0x0C,
    usage=0x01,
    report_ids=(3,),
    in_report_lengths=(2,),
    out_report_lengths=(0,),
)

#: The devices the running interface serves; empty until :func:`enable`
#: or :func:`interface`, as CircuitPython's is until boot.py enables some.
devices = ()


class _HIDInterface(HIDInterface):
    def __init__(self, devs, interval_ms):
        out_max = max(max(d.out_report_lengths) for d in devs)
        in_max = max(max(d.in_report_lengths) for d in devs)
        super().__init__(
            b"".join(d.report_descriptor for d in devs),
            set_report_buf=bytearray(out_max + 1) if out_max else None,
            interval_ms=interval_ms,
        )
        self._devices = devs
        self._tx = bytearray(in_max + 1)
        self._txv = memoryview(self._tx)

    def _send(self, report_id, report):
        # Wait for the last report to go before refilling the one buffer:
        # the transfer reads it until the host has it.
        deadline = time.ticks_add(time.ticks_ms(), _EP_BUSY_WAIT_MS)
        while self.busy():
            if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                raise OSError("USB HID: the host isn't taking reports")
            time.sleep_ms(1)
        n = len(report)
        if self.protocol == 0:
            # Boot protocol (a host's SET_PROTOCOL 0, as a BIOS or a board's
            # simple host asks for): the boot device's report alone, without
            # its ID, which is what the host parses; the others aren't sent.
            if report_id not in self._devices[0].report_ids:
                return
            self._tx[0:n] = report
            self.submit_xfer(self._int_ep, self._txv[:n])
            return
        self._tx[0] = report_id
        self._tx[1 : n + 1] = report
        self.submit_xfer(self._int_ep, self._txv[: n + 1])

    def on_set_report(self, report_data, report_id, report_type):
        data = bytes(report_data)
        # A report with an ID arrives with the ID as its first byte.
        if report_id and data and data[0] == report_id:
            data = data[1:]
        for d in self._devices:
            if report_id in d.report_ids:
                d._received[report_id] = data
        return True


def interface(devs=None, *, interval_ms=1):
    """The HID interface serving ``devs`` (keyboard, mouse and consumer
    control when None), for ``usb.device.get().init()``; sets :data:`devices`."""
    global devices
    if devs is None:
        devs = (Device.KEYBOARD, Device.MOUSE, Device.CONSUMER_CONTROL)
    devs = tuple(devs)
    if not devs:
        raise ValueError("no HID devices")
    itf = _HIDInterface(devs, interval_ms)
    for d in devs:
        d._interface = itf
        d._received = {}
    devices = devs
    return itf


def enable(devs=None, boot_device=0):
    """Start the USB device with these HID devices beside the board's serial
    port, as CircuitPython's ``usb_hid.enable()`` in boot.py does."""
    import usb.device

    if boot_device:
        raise ValueError("boot devices are not supported")
    usb.device.get().init(interface(devs), builtin_driver=True)


def disable():
    """Stop serving HID: the USB device goes back to the board's serial port."""
    global devices
    import usb.device

    usb.device.get().init(builtin_driver=True)
    devices = ()
