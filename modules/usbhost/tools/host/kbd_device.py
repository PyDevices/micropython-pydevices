# The DEV-KIT as a USB keyboard on its device port (usb_hid on
# machine.USBDevice, typed with adafruit_hid), for a host board to read.
import time

import usb.device
import usb_hid
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keycode import Keycode

kb = None


def start():
    global kb
    usb.device.get().init(usb_hid.interface((usb_hid.Device.KEYBOARD,)), builtin_driver=True)
    time.sleep_ms(1500)
    kb = Keyboard(usb_hid.devices)


def type_keys(keys=(Keycode.A, Keycode.B, Keycode.C), shift_last=True):
    for i, k in enumerate(keys):
        if shift_last and i == len(keys) - 1:
            kb.press(Keycode.LEFT_SHIFT, k)
        else:
            kb.press(k)
        time.sleep_ms(60)
        kb.release_all()
        time.sleep_ms(60)
    return len(keys)
