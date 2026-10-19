# MicroPython usb.core: the devices plugged into this board's USB host port.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#
#     dev = usb.core.find(idVendor=0x0781)        # a SanDisk drive, say
#     print(dev.manufacturer, dev.product)
#     dev.write(0x02, cbw)                        # bulk OUT
#     n = dev.read(0x81, buf, timeout=1000)       # bulk IN, into buf
#
# The same calls as CircuitPython's usb.core and PyUSB's, so class drivers
# written for either run here: find(), and on a Device ctrl_transfer(),
# read(), write(), set_configuration() and the kernel-driver calls. Each
# transfer blocks until it completes; a timeout raises USBTimeoutError. An
# endpoint's interface is claimed the first time you use it.
#
# The host starts with the first find(), or with start(), which takes
# full_speed=True to hold the port to full speed: on an ESP32-P4, whose port
# is high speed, that's how full- and low-speed devices behind a hub work
# (ESP-IDF's host has no transaction translator). Underneath is ESP-IDF's USB
# host library, driven by usbif's C module.
import errno
import time

import _usbif

from . import util

_ALL = ("cdc", "hid", "msc", "midi", "uac", "uvc", "other")
_SPEEDS = {"low": 1, "full": 2, "high": 3}
_SETTLE_MS = 1500   # a device just plugged in needs this long to enumerate


# Not every port's errno names them all: ESP32 builds have no EPIPE, and a
# missing name here turned every USB error after a timeout into an
# AttributeError. The numbers are MicroPython's (py/mperrno.h), which the C
# module raises.
_ETIMEDOUT = getattr(errno, "ETIMEDOUT", 110)
_EPIPE = getattr(errno, "EPIPE", 32)
_ENODEV = getattr(errno, "ENODEV", 19)
_ENOENT = getattr(errno, "ENOENT", 2)
_EOPNOTSUPP = getattr(errno, "EOPNOTSUPP", 95)


class USBError(OSError):
    pass


class USBTimeoutError(USBError):
    pass


class NoBackendError(ValueError):
    pass


def _raise(e):
    if e.errno == _ETIMEDOUT:
        raise USBTimeoutError(e.errno, "timed out")
    if e.errno == _EPIPE:
        raise USBError(e.errno, "pipe error (stall)")
    if e.errno == _ENODEV:
        raise USBError(e.errno, "no such device (it may have been unplugged)")
    raise USBError(e.errno, str(e))


_started = None
_full_speed = None


_classes = None


def start(full_speed=False, classes=None):
    """Start the host port, or restart it if a setting changed.

    ``classes`` names what this session will stream, for the controller's
    FIFO split, which is fixed while the host runs: ``("uvc",)`` leans to
    IN for a camera, ``("uac",)`` leaves room for a speaker's periodic OUT.
    None asks for everything, which leans to IN."""
    global _started, _full_speed, _classes
    want = tuple(classes) if classes else _ALL
    if _started is not None and full_speed == _full_speed and want == _classes:
        return
    if _started is not None:
        _usbif.host_stop()
    try:
        _usbif.host_start(want + ("other",) if "other" not in want else want, full_speed=full_speed)
    except AttributeError:
        raise NoBackendError("this firmware has no USB host")
    _started = time.ticks_ms()
    _full_speed = full_speed
    _classes = want


def stop():
    """Stop the host port; the next find() or start() starts it again."""
    global _started, _full_speed
    if _started is not None:
        _usbif.host_stop()
    _started = _full_speed = _classes = None


def running():
    """Whether the host port is running: True from start() (or the first
    find()) until stop(), as the C host reports it."""
    if _started is None:
        return False
    st = _usbif.host_stats()
    return bool(st and st[0])


def stats():
    """The host's counters, as a dict: ``running``, ``attaches``,
    ``detaches``, ``errors`` (enumeration failures), ``devices`` open in
    the host library, and the ``full_speed`` and ``classes`` it was started
    with. None when this firmware has no USB host."""
    try:
        st = _usbif.host_stats()
    except AttributeError:
        return None
    if not st:
        return None
    return {"running": bool(st[0]) and _started is not None, "attaches": st[1], "detaches": st[2],
            "errors": st[3], "devices": st[4], "full_speed": bool(_full_speed), "classes": _classes}


def _start():
    if _started is None:
        start()
    wait = _SETTLE_MS - time.ticks_diff(time.ticks_ms(), _started)
    if wait > 0:
        time.sleep_ms(wait)


def find(find_all=False, *, idVendor=None, idProduct=None, custom_match=None):
    """The first device that matches, or None; with find_all, an iterator of
    all of them."""
    _start()
    found = []
    for row in _usbif.host_devices():
        dev = Device(row)
        if idVendor is not None and dev.idVendor != idVendor:
            continue
        if idProduct is not None and dev.idProduct != idProduct:
            continue
        if custom_match is not None and not custom_match(dev):
            continue
        if not find_all:
            return dev
        found.append(dev)
    return iter(found) if find_all else None


class Device:
    def __init__(self, row):
        self._id = row[0]
        self.idVendor = row[1]
        self.idProduct = row[2]
        self.speed = _SPEEDS.get(row[6], 0)
        self._claims = {}       # interface -> alternate setting
        self._ep_itf = {}       # endpoint -> its interface, read once
        self._alts = {}         # alternates chosen by set_interface_altsetting
        self._desc = None
        self._strings = {}

    def __repr__(self):
        return "<Device %04x:%04x>" % (self.idVendor, self.idProduct)

    # -- descriptors

    def _device_desc(self):
        if self._desc is None:
            buf = bytearray(18)
            self.ctrl_transfer(0x80, 6, 0x0100, 0, buf, 1000)
            self._desc = bytes(buf)
        return self._desc

    @property
    def bcdDevice(self):
        d = self._device_desc()
        return d[12] | d[13] << 8

    @property
    def bDeviceClass(self):
        return self._device_desc()[4]

    def _string(self, offset):
        index = self._device_desc()[offset]
        if not index:
            return None
        if index not in self._strings:
            self._strings[index] = util.get_string(self, index)
        return self._strings[index]

    @property
    def manufacturer(self):
        return self._string(14)

    @property
    def product(self):
        return self._string(15)

    @property
    def serial_number(self):
        return self._string(16)

    def configuration_descriptor(self):
        """The active configuration descriptor, every byte of it."""
        return _usbif.host_desc(self._id)

    # -- configuration and interfaces

    def set_configuration(self, configuration=None):
        # ESP-IDF's host sets the first configuration at enumeration, which
        # is the one nearly every device has.
        if configuration not in (None, 1):
            raise USBError(_EOPNOTSUPP, "only the first configuration is supported")

    def set_interface_altsetting(self, interface=0, alternate_setting=0):
        """Select an alternate setting, as PyUSB does: the host claims the
        interface at it (its endpoints become usable here) and the device is
        told with SET_INTERFACE, which ESP-IDF's host never sends itself. An
        audio or video interface streams only on a nonzero alternate."""
        self._alts[interface] = alternate_setting
        self._claim(interface)
        self.ctrl_transfer(0x01, 0x0B, alternate_setting, interface, None, 3000)

    def is_kernel_driver_active(self, interface):
        return False

    def detach_kernel_driver(self, interface):
        pass

    def attach_kernel_driver(self, interface):
        self._release(interface)

    def _claim(self, interface):
        alt = self._alts.get(interface, 0)
        if self._claims.get(interface) == alt:
            return
        try:
            fresh = _usbif.host_claim(self._id, interface, alt)
        except OSError as e:
            _raise(e)
        self._claims[interface] = alt
        # A fresh claim gives the host new pipes, expecting DATA0, while the
        # device's endpoints carry on from wherever their toggles were: a
        # second claim then loses the first packet each way, or a whole
        # reply. CLEAR_FEATURE(ENDPOINT_HALT) puts the device's back to
        # DATA0 too, as libusb's clear_halt does. An interface another
        # Device object already claimed keeps its pipes, and its toggles.
        if fresh:
            for ep in self._endpoints(interface, alt):
                self.ctrl_transfer(0x02, 0x01, 0, ep, None, 1000)

    def _release(self, interface):
        if self._claims.pop(interface, None) is not None:
            try:
                _usbif.host_release(self._id, interface)
            except OSError as e:
                _raise(e)

    def _release_all(self):
        for interface in list(self._claims):
            self._release(interface)

    def _endpoints(self, interface, alt):
        """The bulk and interrupt endpoints of one alternate setting."""
        cfg = self.configuration_descriptor()
        i, here, eps = cfg[0], False, []
        while i + 2 <= len(cfg) and cfg[i] >= 2:
            if cfg[i + 1] == 4:
                here = cfg[i + 2] == interface and cfg[i + 3] == alt
            elif cfg[i + 1] == 5 and here and cfg[i + 3] & 3 in (2, 3):
                eps.append(cfg[i + 2])
            i += cfg[i]
        return eps

    def _interface_of(self, endpoint):
        itf = self._ep_itf.get(endpoint)
        if itf is None:
            itf = self._ep_itf[endpoint] = self._find_interface(endpoint)
        return itf

    def _find_interface(self, endpoint):
        cfg = self.configuration_descriptor()
        i, itf = cfg[0], None
        while i + 2 <= len(cfg) and cfg[i] >= 2:
            if cfg[i + 1] == 4:
                itf = cfg[i + 2]
            elif cfg[i + 1] == 5 and cfg[i + 2] == endpoint:
                return itf
            i += cfg[i]
        raise USBError(_ENOENT, "no endpoint 0x%02x" % endpoint)

    # -- transfers

    def ctrl_transfer(self, bmRequestType, bRequest, wValue=0, wIndex=0, data_or_wLength=None, timeout=None):
        """A control transfer. For a request to the host (IN), pass a buffer
        to read into, and get the count, or a length, and get an array of
        what came. For one to the device, pass the data, or nothing."""
        into = None
        if bmRequestType & 0x80:
            if isinstance(data_or_wLength, int):
                into = buf = bytearray(data_or_wLength)
            else:
                buf = data_or_wLength if data_or_wLength is not None else bytearray(0)
        else:
            buf = data_or_wLength if data_or_wLength is not None else b""
        setup = bytes((bmRequestType, bRequest, wValue & 0xFF, wValue >> 8, wIndex & 0xFF, wIndex >> 8,
                       len(buf) & 0xFF, len(buf) >> 8))
        try:
            n = _usbif.host_ctrl(self._id, setup, buf, timeout or 0)
        except OSError as e:
            _raise(e)
        if into is not None:
            return util._array(into, n)
        return n

    def write(self, endpoint, data, timeout=None):
        """Send data to a bulk or interrupt OUT endpoint; returns the count."""
        self._claim(self._interface_of(endpoint))
        try:
            return _usbif.host_xfer(self._id, endpoint, data, timeout or 0)
        except OSError as e:
            _raise(e)

    def read(self, endpoint, size_or_buffer, timeout=None, *, raise_on_timeout=True):
        """Read from a bulk or interrupt IN endpoint: into a buffer, giving
        the count, or up to a size, giving an array. timeout None or 0 waits
        for data, a quarter-second at a time so Ctrl-C still works.

        With ``raise_on_timeout=False`` (as in CircuitPython), a timeout
        returns 0 instead of raising USBTimeoutError, and a read into a
        buffer allocates nothing: poll an interrupt endpoint with a short
        timeout and "no report yet" costs no garbage. The transfer stays
        armed between such reads, so a report that lands in between is
        kept for the next one."""
        self._claim(self._interface_of(endpoint))
        into = None
        if isinstance(size_or_buffer, int):
            into = buf = bytearray(size_or_buffer)
        else:
            buf = size_or_buffer
        if not raise_on_timeout and timeout:
            try:
                n = _usbif.host_xfer(self._id, endpoint, buf, timeout, False)
            except OSError as e:
                _raise(e)
            return util._array(into, n) if into is not None else n
        while True:
            try:
                n = _usbif.host_xfer(self._id, endpoint, buf, timeout or 250)
                break
            except OSError as e:
                if not timeout and e.errno == _ETIMEDOUT:
                    continue
                _raise(e)
        if into is not None:
            return util._array(into, n)
        return n
