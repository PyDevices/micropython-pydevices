# MicroPython USB HID host: reports from a keyboard or mouse on the host port.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#     from usb.host.hid import Device, KEYBOARD
#
#     kbd = Device(usb.core.find(custom_match=Device.match(KEYBOARD)), KEYBOARD)
#     while True:
#         report = kbd.read()          # 8 bytes, or None when nothing came
#         if report:
#             print(report[0], report[2:])   # modifiers, keys held
#
# Boot protocol, which every keyboard and mouse speaks without a report
# descriptor being parsed: a keyboard's report is the modifier byte, a
# reserved byte and up to six usage IDs held; a mouse's is buttons, x, y
# (and usually a wheel). Device switches the interface to boot protocol and
# asks for reports only on change (SET_IDLE 0). For the descriptor, to read
# a device in report protocol, see report_descriptor().
import usb.core

KEYBOARD = 1
MOUSE = 2

_SET_IDLE = 0x0A
_SET_PROTOCOL = 0x0B
_GET_PROTOCOL = 0x03
_SET_REPORT = 0x09


def _interfaces(dev):
    """(interface, protocol, interrupt IN, IN packet size, interrupt OUT) of
    each HID interface, in order."""
    cfg = dev.configuration_descriptor()
    i, cur, out = cfg[0], None, []
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4:
            cur = None
            if cfg[i + 5] == 0x03 and cfg[i + 3] == 0:
                cur = [cfg[i + 2], cfg[i + 7] if cfg[i + 6] == 1 else 0, None, 8, None]
                out.append(cur)
        elif cfg[i + 1] == 5 and cur is not None and cfg[i + 3] & 3 == 3:
            if cfg[i + 2] & 0x80:
                cur[2], cur[3] = cfg[i + 2], cfg[i + 4] | cfg[i + 5] << 8
            else:
                cur[4] = cfg[i + 2]
        i += cfg[i]
    return [tuple(x) for x in out if x[2]]


class Device:
    @staticmethod
    def match(protocol=KEYBOARD):
        """For usb.core.find(custom_match=...): a device with a boot keyboard
        (or mouse) interface."""
        return lambda dev: any(x[1] == protocol for x in _interfaces(dev))

    def __init__(self, dev, protocol=KEYBOARD, boot=True):
        if dev is None:
            raise ValueError("no device")
        found = [x for x in _interfaces(dev) if x[1] == protocol] or _interfaces(dev)
        if not found:
            raise ValueError("no HID interface")
        self.dev = dev
        self.interface, self.protocol, self._in, size, self._out = found[0]
        self._packet = size
        self._buf = bytearray(64)
        self._mv = memoryview(self._buf)
        dev.set_configuration()
        # Boot protocol only on a boot interface; its reports fit one packet.
        self.boot = bool(boot and self.protocol)
        if self.protocol:
            self._class_out(_SET_PROTOCOL, 0 if boot else 1)   # boot, or report
        try:
            self._class_out(_SET_IDLE, 0)   # report only on change
        except usb.core.USBError:
            pass    # optional for a mouse, and some stall it

    def _class_out(self, request, value, data=None):
        self.dev.ctrl_transfer(0x21, request, value, self.interface, data, 1000)

    def boot_protocol(self):
        """True when the interface is in boot protocol."""
        return self.dev.ctrl_transfer(0xA1, _GET_PROTOCOL, 0, self.interface, 1, 1000)[0] == 0

    def report_descriptor(self, length=256):
        return bytes(self.dev.ctrl_transfer(0x81, 6, 0x2200, self.interface, length, 1000))

    def read(self, timeout=1):
        """The next report, or None if none came within timeout ms. The
        bytes are reused by the next read; copy them to keep them."""
        # A packet at a time. A report that fills its last packet exactly (a
        # boot keyboard's 8 bytes) sends no short packet after it, so a read
        # asking for more would wait for the next report. In boot protocol a
        # report is one packet. In report protocol one can be longer (with a
        # report ID, a keyboard's 9 bytes), so after a full packet, a brief
        # look for the rest.
        p = self._packet
        n = 0
        try:
            n = self.dev.read(self._in, self._mv[:p], timeout)
            while not self.boot and n and n % p == 0 and n + p <= len(self._buf):
                more = self.dev.read(self._in, self._mv[n:n + p], 2)
                n += more
                if more < p:
                    break
        except usb.core.USBTimeoutError:
            if not n:
                return None
        return self._mv[:n] if n else None

    def set_leds(self, leds):
        """A keyboard's LEDs: bit 0 Num Lock, 1 Caps Lock, 2 Scroll Lock."""
        if self._out:
            self.dev.write(self._out, bytes((leds,)), 1000)
        else:
            self._class_out(_SET_REPORT, 0x0200, bytes((leds,)))

    def close(self):
        self.dev._release_all()
