# MicroPython USB network host: a phone's USB tethering as a network interface.
# MIT license; Copyright (c) 2026 Brad Barnett
#
#     import usb.core
#     from usb.host.net import Tether
#
#     net = Tether()              # waits for a tethering phone on the host port
#     net.active(True)            # DHCP from the phone
#     while not net.isconnected():
#         time.sleep_ms(100)
#     print(net.ifconfig())       # then socket, requests and mip work over it
#
# Turn on USB tethering on an Android phone (Settings > Connections > Mobile
# Hotspot and Tethering > USB tethering) and the phone presents an RNDIS
# network adapter. This finds it, does RNDIS's handshake (initialise, read the
# MAC address, set the packet filter) and brings up a network interface on it,
# with its address, gateway and DNS from the phone. Frames move in C; DHCP,
# DNS and sockets are ESP-IDF's, as for Wi-Fi or Ethernet. The phone shares
# its mobile data, or on many phones its Wi-Fi.
import struct
import time

import _usbif
import usb.core

_SEND = 0x00            # SEND_ENCAPSULATED_COMMAND
_GET = 0x01             # GET_ENCAPSULATED_RESPONSE
_INITIALIZE = 0x00000002
_QUERY = 0x00000004
_SET = 0x00000005
_OID_PERMANENT_ADDRESS = 0x01010101
_OID_PACKET_FILTER = 0x0001010E
_FILTER = 0x01 | 0x04 | 0x08    # directed, all multicast, broadcast


def _rndis(dev):
    """(control interface, data interface, bulk IN, bulk OUT, packet), or None."""
    cfg = dev.configuration_descriptor()
    i, ctrl, data, cur, ep_in, ep_out, mps = cfg[0], None, None, None, None, None, 64
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4:
            cur = cfg[i + 2]
            klass = (cfg[i + 5], cfg[i + 6], cfg[i + 7])
            if ctrl is None and klass in ((0xE0, 0x01, 0x03), (0x02, 0x02, 0xFF), (0xEF, 0x04, 0x01)):
                ctrl = cur
            elif ctrl is not None and data is None and cfg[i + 5] == 0x0A:
                data = cur
        elif cfg[i + 1] == 5 and cur is not None and cur == data and cfg[i + 3] & 3 == 2:
            if cfg[i + 2] & 0x80:
                ep_in, mps = cfg[i + 2], cfg[i + 4] | cfg[i + 5] << 8
            else:
                ep_out = cfg[i + 2]
        i += cfg[i]
    if ctrl is None or data is None or ep_in is None or ep_out is None:
        return None
    return ctrl, data, ep_in, ep_out, mps


class Tether:
    @staticmethod
    def match(dev):
        """For usb.core.find(custom_match=...): an RNDIS network adapter."""
        return _rndis(dev) is not None

    def __init__(self, dev=None, *, wait_ms=10000):
        if dev is None:
            # A camera's FIFO split leaves 256 bytes for bulk OUT, and a phone
            # sends 512-byte packets at high speed: ask for a CDC session.
            usb.core.start(classes=("cdc",))
            deadline = time.ticks_add(time.ticks_ms(), wait_ms)
            while dev is None:
                dev = usb.core.find(custom_match=self.match)
                if dev is None:
                    if time.ticks_diff(deadline, time.ticks_ms()) <= 0:
                        raise OSError(19, "no USB tethering device: turn on USB tethering on the phone")
                    time.sleep_ms(250)
        found = _rndis(dev)
        if found is None:
            raise ValueError("not an RNDIS network adapter")
        self.dev = dev
        self._ctrl, self._data, self._in, self._out, self._mps = found
        self._req = 0
        self.mac = None
        self._up = False

    # -- RNDIS control

    def _command(self, msg_type, body):
        self._req += 1
        msg = struct.pack("<III", msg_type, 12 + len(body), self._req) + body
        self.dev.ctrl_transfer(0x21, _SEND, 0, self._ctrl, msg, 1000)
        buf = bytearray(1025)
        for _ in range(50):
            try:
                n = self.dev.ctrl_transfer(0xA1, _GET, 0, self._ctrl, buf, 1000)
            except usb.core.USBError:
                n = 0
            if n >= 16:
                rtype, rlen, rid, status = struct.unpack_from("<IIII", buf)
                if rtype == (msg_type | 0x80000000) and rid == self._req:
                    if status != 0:
                        raise OSError(5, "RNDIS message %x failed: status %x" % (msg_type, status))
                    return bytes(buf[:rlen])
            time.sleep_ms(20)
        raise OSError(110, "no RNDIS response to message %x" % msg_type)

    def _query(self, oid):
        r = self._command(_QUERY, struct.pack("<IIII", oid, 0, 20, 0))
        length, offset = struct.unpack_from("<II", r, 16)
        return r[8 + offset:8 + offset + length]

    def _set(self, oid, value):
        self._command(_SET, struct.pack("<IIII", oid, len(value), 20, 0) + value)

    # -- the interface, as network.LAN has it

    def active(self, on=None):
        if on is None:
            return self._up
        if on and not self._up:
            # Version 1.0; the largest transfer this host takes in one go, so the
            # phone can pack several frames into each.
            self._command(_INITIALIZE, struct.pack("<III", 1, 0, 8192))
            self.mac = self._query(_OID_PERMANENT_ADDRESS)[:6]
            self._set(_OID_PACKET_FILTER, struct.pack("<I", _FILTER))
            _usbif.host_net_start(self.dev._id, self._data, self._in, self._out, self._mps, self.mac, True)
            self._up = True
        elif not on and self._up:
            _usbif.host_net_stop()
            self._up = False
        return self._up

    def _info(self):
        return _usbif.host_net_info()

    def isconnected(self):
        """True once the phone has given this board an address."""
        info = self._info()
        return bool(info and info[0] and info[1][0] != "0.0.0.0")

    def ifconfig(self):
        """(address, netmask, gateway, DNS)."""
        info = self._info()
        return info[1] if info else ("0.0.0.0",) * 4

    def config(self, name):
        if name == "mac":
            return self.mac
        raise ValueError("unknown config: %s" % name)

    def set_default(self):
        """Send sockets through this interface when nothing else routes them."""
        _usbif.host_net_default()

    def stats(self):
        """(rx frames, tx frames, rx dropped, tx dropped, rx errors)."""
        info = self._info()
        return info[2] if info else None
