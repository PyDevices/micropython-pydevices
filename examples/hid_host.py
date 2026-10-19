"""A USB keyboard on the host port drives ordinary PyDevices key events.

The same application code, producing the same ``events.Key`` records, as an
SDL keyboard on the desktop. ``usb.host.hid`` delivers the keyboard's raw
boot reports; ``KeyboardDecoder`` below turns them into presses and releases
by diffing successive reports.

    mpftp run -d COM49 examples/hid_host.py

**Pairing.** A commercial keyboard, or a PyDevices board running
``hid_keyboard.py``. Needs an S3, or a P4 with the keyboard on a host port
that supplies 5 V. Behind a hub on a P4, set ``FULL_SPEED = True`` below: a
high-speed hub can't reach a full-speed keyboard without a transaction
translator, which the ESP-IDF host lacks.

**Lock keys.** A keyboard on a host doesn't light its own Caps Lock: the
host tells it to. Pressing Caps, Num or Scroll Lock here toggles that LED
with ``set_leds()``.

**Rollover.** When more keys are held than the report can carry, a keyboard
fills every slot with ErrorRollOver (``0x01``). The decoder ignores that
report rather than emitting six garbage keys, and leaves the held set alone
so the eventual releases still balance.
"""

import time

import events
import keys
import usb.core
from usb.host.hid import KEYBOARD, Device

# True when the device is behind a hub on an ESP32-P4: the host then runs at
# full speed, so the hub passes a full-speed device through. Plugged straight
# into the port, or on an S3, leave it False.
FULL_SPEED = False

_ERROR_ROLLOVER = 0x01

# HID usage of each lock key, and its bit in the keyboard's LED report.
_LOCKS = {0x53: 0x01, 0x39: 0x02, 0x47: 0x04}   # Num, Caps, Scroll


class KeyboardDecoder:
    """Diffs successive boot reports into ``events.Key`` records.

    A boot report says which keys are held now, never "pressed" or
    "released": byte 0 is the modifier bits, byte 1 is reserved, bytes 2-7
    are up to six HID usages held. Events come from comparing one report
    with the last, so use one decoder per keyboard.

    Modifiers come only as bits, never in the usage list, so Shift pressed
    alone sends a report with no usages at all. They're diffed too, and
    come out as their own key events, as SDL gives them, with their usages
    (0xE0-0xE7) as the scancode.
    """

    def __init__(self):
        self.held = frozenset()
        self._mod_bits = 0

    def feed(self, report):
        """The events one report means: releases first, then presses, with
        a modifier down before the key it modifies and up after it."""
        if len(report) < 3:
            return ()
        usages = report[2:8]
        if usages[0] == _ERROR_ROLLOVER and all(u in (0, _ERROR_ROLLOVER) for u in usages):
            return ()
        now = frozenset(u for u in usages if u > _ERROR_ROLLOVER)
        bits = report[0]
        mod = keys.hid_modifiers(bits)
        changed = self._mod_bits ^ bits
        out = []
        for i, (_kmod, code) in enumerate(keys.HID_MODIFIERS):
            if changed & ~bits & (1 << i):
                out.append(events.Key(events.KEYUP, keys.keyname(code), code, mod, 0xE0 + i, None))
        for usage in sorted(self.held - now):
            out.append(self._key(events.KEYUP, usage, mod))
        for usage in sorted(now - self.held):
            out.append(self._key(events.KEYDOWN, usage, mod))
        for i, (_kmod, code) in enumerate(keys.HID_MODIFIERS):
            if changed & bits & (1 << i):
                out.append(events.Key(events.KEYDOWN, keys.keyname(code), code, mod, 0xE0 + i, None))
        self.held = now
        self._mod_bits = bits
        return out

    @staticmethod
    def _key(kind, usage, mod):
        code = keys.hid_keycode(usage)
        name = keys.keyname(code) if code is not None else "Unknown"
        # The HID usage is the hardware scancode; no window, as this didn't
        # come from a display.
        return events.Key(kind, name, code, mod, usage, None)


def find_keyboard(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Device.match(KEYBOARD))
        if dev is not None:
            return dev
        time.sleep_ms(250)
    return None


def keyboard_report(buf, n):
    """The 8-byte boot keyboard report in the first ``n`` bytes, or None.

    A plain keyboard sends 8 bytes. One that shares its interface with a
    mouse (a PyDevices board running ``hid_keyboard.py`` is one) puts a
    report ID in front, so its keyboard reports are 9 bytes; read as boot
    reports unstripped, the ID lands in the modifier byte and every key
    comes with Left Ctrl held. Anything else, such as that interface's
    mouse reports, isn't a keyboard report.
    """
    if n == 8:
        return bytes(buf[:8])
    if n == 9:
        return bytes(buf[1:9])
    return None


def main(seconds=30):
    usb.core.start(full_speed=FULL_SPEED, classes=("hid",))
    dev = find_keyboard()
    if dev is None:
        print("no keyboard found")
        usb.core.stop()
        return

    kbd = Device(dev, protocol=KEYBOARD, boot=True)
    print("keyboard %04x:%04x %s %s -- type on it for %d s"
          % (dev.idVendor, dev.idProduct, dev.manufacturer, dev.product, seconds))
    decoder = KeyboardDecoder()
    leds = 0
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            raw = kbd.read(timeout=20)
            report = keyboard_report(raw, len(raw)) if raw else None
            if report is None:
                continue
            for ev in decoder.feed(report):
                kind = "DOWN" if ev.type == events.KEYDOWN else "UP"
                print("  %s %-12s key=0x%02x mod=0x%02x scancode=%s"
                      % (kind, ev.name, ev.key or 0, ev.mod, ev.scancode))
                if ev.type == events.KEYDOWN and ev.scancode in _LOCKS:
                    leds ^= _LOCKS[ev.scancode]
                    try:
                        kbd.set_leds(leds)
                    except usb.core.USBError:
                        pass    # a keyboard with no LEDs may refuse the report
    finally:
        kbd.close()
        usb.core.stop()
        print("done")


if __name__ == "__main__":
    main()
