"""A USB keyboard on the host port drives ordinary PyDevices key events.

The same application code, producing the same ``events.Key`` records, as an
SDL keyboard on the desktop. ``usb.host.hid`` delivers the keyboard's key
presses and releases as ``adafruit_hid`` Keycode values; ``KeyboardEvents``
below turns each into the matching ``events.Key``.

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
fills every slot with ErrorRollOver (``0x01``). ``usb.host.hid`` ignores that
report and keeps what it had, so the eventual releases still balance.
"""

import time

import events
import keys
import usb.core
from usb.host.hid import KEY_DOWN, KEY_UP, KEYBOARD, Device

# True when the device is behind a hub on an ESP32-P4: the host then runs at
# full speed, so the hub passes a full-speed device through. Plugged straight
# into the port, or on an S3, leave it False.
FULL_SPEED = False

# HID usage of each lock key, and its bit in the keyboard's LED report.
_LOCKS = {0x53: 0x01, 0x39: 0x02, 0x47: 0x04}   # Num, Caps, Scroll


class KeyboardEvents:
    """Turns ``usb.host.hid`` events into ``events.Key`` records.

    A key event carries the key's HID usage (an ``adafruit_hid`` Keycode) and
    the modifiers held after its report. Modifier keys are events of their
    own (usages 0xE0-0xE7), as SDL gives them, with the usage as the
    scancode. Mouse and media-key events aren't keys and come out as None.
    """

    def feed(self, ev):
        kind, usage = ev[0], ev[1]
        if kind != KEY_DOWN and kind != KEY_UP:
            return None
        pressed = events.KEYDOWN if kind == KEY_DOWN else events.KEYUP
        mod = keys.hid_modifiers(ev[2])
        if 0xE0 <= usage <= 0xE7:
            code = keys.HID_MODIFIERS[usage - 0xE0][1]
            return events.Key(pressed, keys.keyname(code), code, mod, usage, None)
        code = keys.hid_keycode(usage)
        name = keys.keyname(code) if code is not None else "Unknown"
        # The HID usage is the hardware scancode; no window, as this didn't
        # come from a display.
        return events.Key(pressed, name, code, mod, usage, None)


def find_keyboard(timeout_ms=15000):
    deadline = time.ticks_add(time.ticks_ms(), timeout_ms)
    while time.ticks_diff(deadline, time.ticks_ms()) > 0:
        dev = usb.core.find(custom_match=Device.match(KEYBOARD))
        if dev is not None:
            return dev
        time.sleep_ms(250)
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
    decoder = KeyboardEvents()
    leds = 0
    t0 = time.ticks_ms()
    try:
        while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
            raw = kbd.read(20)
            ev = decoder.feed(raw) if raw else None
            if ev is None:
                continue
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
