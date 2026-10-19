# MicroPython usb.util: constants and helpers beside usb.core, as PyUSB's.
# MIT license; Copyright (c) 2026 Brad Barnett
from array import array

from micropython import const

DESC_TYPE_DEVICE = const(0x01)
DESC_TYPE_CONFIG = const(0x02)
DESC_TYPE_STRING = const(0x03)
DESC_TYPE_INTERFACE = const(0x04)
DESC_TYPE_ENDPOINT = const(0x05)

ENDPOINT_IN = const(0x80)
ENDPOINT_OUT = const(0x00)
ENDPOINT_TYPE_CTRL = const(0)
ENDPOINT_TYPE_ISO = const(1)
ENDPOINT_TYPE_BULK = const(2)
ENDPOINT_TYPE_INTR = const(3)

CTRL_IN = const(0x80)
CTRL_OUT = const(0x00)
CTRL_TYPE_STANDARD = const(0x00)
CTRL_TYPE_CLASS = const(0x20)
CTRL_TYPE_VENDOR = const(0x40)
CTRL_TYPE_RESERVED = const(0x60)
CTRL_RECIPIENT_DEVICE = const(0)
CTRL_RECIPIENT_INTERFACE = const(1)
CTRL_RECIPIENT_ENDPOINT = const(2)
CTRL_RECIPIENT_OTHER = const(3)

SPEED_LOW = const(1)
SPEED_FULL = const(2)
SPEED_HIGH = const(3)


def _array(buf, n):
    a = array("B", buf)
    return a[:n] if n < len(a) else a


def endpoint_address(address):
    return address & 0x0F


def endpoint_direction(address):
    return address & 0x80


def endpoint_type(bmAttributes):
    return bmAttributes & 0x03


def ctrl_direction(bmRequestType):
    return bmRequestType & 0x80


def build_request_type(direction, type, recipient):
    return direction | type | recipient


def get_langids(dev):
    buf = bytearray(255)
    n = dev.ctrl_transfer(0x80, 6, DESC_TYPE_STRING << 8, 0, buf, 1000)
    return tuple(buf[i] | buf[i + 1] << 8 for i in range(2, n - 1, 2))


def get_string(dev, index, langid=None):
    """String descriptor `index` of dev, decoded; None for index 0."""
    if not index:
        return None
    if langid is None:
        ids = get_langids(dev)
        langid = ids[0] if ids else 0x0409
    buf = bytearray(255)
    n = dev.ctrl_transfer(0x80, 6, DESC_TYPE_STRING << 8 | index, langid, buf, 1000)
    # UTF-16LE; MicroPython has no utf-16 codec, so the basic plane by hand.
    return "".join(chr(buf[i] | buf[i + 1] << 8) for i in range(2, min(n, buf[0]) - 1, 2))


def claim_interface(dev, interface):
    dev._claim(interface)


def release_interface(dev, interface):
    dev._release(interface)


def dispose_resources(dev):
    dev._release_all()
