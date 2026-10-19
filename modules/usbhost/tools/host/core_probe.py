# Board half of the usb.core check: list what's plugged into the host port,
# then talk to a flash drive with nothing but usb.core's bulk transfers
# (SCSI INQUIRY and READ CAPACITY over Bulk-Only Transport), and to a MIDI
# device by reading its bulk IN endpoint for a while. Driven over a held REPL.
import struct
import time

import usb.core
import usb.util


def devices():
    out = []
    for dev in usb.core.find(find_all=True):
        try:
            names = (dev.manufacturer, dev.product)
        except usb.core.USBError as e:
            names = ("?", repr(e))
        out.append(("%04x:%04x" % (dev.idVendor, dev.idProduct), dev.speed, names, _classes(dev)))
    return out


def _walk(dev):
    cfg = dev.configuration_descriptor()
    i, itf = cfg[0], None
    while i + 2 <= len(cfg) and cfg[i] >= 2:
        if cfg[i + 1] == 4:
            itf = (cfg[i + 2], cfg[i + 3], cfg[i + 5], cfg[i + 6], cfg[i + 7])
            yield "itf", itf
        elif cfg[i + 1] == 5:
            yield "ep", (itf, cfg[i + 2], cfg[i + 3] & 3, cfg[i + 4] | cfg[i + 5] << 8)
        i += cfg[i]


def _classes(dev):
    return sorted({v[2] for k, v in _walk(dev) if k == "itf"})


def _bulk_pair(dev, klass, subclass=None):
    ep_in = ep_out = itf = None
    for k, v in _walk(dev):
        if k == "ep" and v[0] and v[0][2] == klass and (subclass is None or v[0][3] == subclass) and v[2] == 2:
            itf = v[0][0]
            if v[1] & 0x80:
                ep_in = ep_in or v[1]
            else:
                ep_out = ep_out or v[1]
    return itf, ep_in, ep_out


def _scsi(dev, ep_in, ep_out, tag, cb, length):
    cbw = struct.pack("<IIIBBB", 0x43425355, tag, length, 0x80 if length else 0, 0, len(cb)) + cb + bytes(16 - len(cb))
    dev.write(ep_out, cbw, timeout=2000)
    data = bytearray(length)
    n = dev.read(ep_in, data, timeout=2000) if length else 0
    csw = bytearray(13)
    dev.read(ep_in, csw, timeout=2000)
    sig, rtag, residue, status = struct.unpack("<IIIB", csw)
    return bytes(data[:n]), status, sig == 0x53425355 and rtag == tag


def drive():
    for dev in usb.core.find(find_all=True):
        itf, ep_in, ep_out = _bulk_pair(dev, 0x08)
        if itf is None:
            continue
        t0 = time.ticks_ms()
        inq, st1, ok1 = _scsi(dev, ep_in, ep_out, 1, bytes((0x12, 0, 0, 0, 36, 0)), 36)
        cap, st2, ok2 = _scsi(dev, ep_in, ep_out, 2, bytes((0x25,)) + bytes(9), 8)
        last, size = struct.unpack(">II", cap) if len(cap) == 8 else (0, 0)
        # One READ(10) of 64 blocks from LBA 0, timed.
        blocks = 64
        t1 = time.ticks_ms()
        data, st3, ok3 = _scsi(dev, ep_in, ep_out, 3, struct.pack(">BBIBHB", 0x28, 0, 0, 0, blocks, 0), blocks * size)
        ms = time.ticks_diff(time.ticks_ms(), t1)
        dev._release_all()
        return {"vendor": inq[8:16].decode().strip(), "product": inq[16:32].decode().strip(),
                "status": (st1, st2, st3), "csw_ok": ok1 and ok2 and ok3, "blocks": last + 1, "block_size": size,
                "read_bytes": len(data), "read_ms": ms, "mbr_sig": data[510:512].hex() if len(data) >= 512 else None,
                "total_ms": time.ticks_diff(time.ticks_ms(), t0)}
    return None


def midi(seconds=5):
    for dev in usb.core.find(find_all=True):
        itf, ep_in, ep_out = _bulk_pair(dev, 0x01, 0x03)
        if itf is None:
            continue
        got, packets = [], 0
        buf = bytearray(64)
        end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            try:
                n = dev.read(ep_in, buf, timeout=100)
            except usb.core.USBTimeoutError:
                continue
            for i in range(0, n, 4):
                if buf[i] & 0x0F:
                    packets += 1
                    if len(got) < 8:
                        got.append(bytes(buf[i:i + 4]).hex())
        dev._release_all()
        return {"device": "%04x:%04x" % (dev.idVendor, dev.idProduct), "packets": packets, "first": got}
    return None


def identity():
    """MIDI Identity Request out of the bulk OUT endpoint, and the device's
    Identity Reply back from bulk IN: a round trip with no one at the keys."""
    for dev in usb.core.find(find_all=True):
        itf, ep_in, ep_out = _bulk_pair(dev, 0x01, 0x03)
        if itf is None or ep_out is None:
            continue
        buf = bytearray(64)
        while True:     # drain anything already waiting
            try:
                dev.read(ep_in, buf, timeout=50)
            except usb.core.USBTimeoutError:
                break
        t0 = time.ticks_us()
        dev.write(ep_out, bytes((0x04, 0xF0, 0x7E, 0x7F, 0x07, 0x06, 0x01, 0xF7)), timeout=1000)
        reply = bytearray()
        end = time.ticks_add(time.ticks_ms(), 1000)
        while time.ticks_diff(end, time.ticks_ms()) > 0 and not reply.endswith(b"\xf7"):
            try:
                n = dev.read(ep_in, buf, timeout=200)
            except usb.core.USBTimeoutError:
                continue
            for i in range(0, n, 4):
                cin = buf[i] & 0x0F
                take = {0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3}.get(cin, 0)
                reply += buf[i + 1:i + 1 + take]
        us = time.ticks_diff(time.ticks_us(), t0)
        dev._release_all()
        return {"device": "%04x:%04x" % (dev.idVendor, dev.idProduct), "reply": bytes(reply).hex(), "us": us}
    return None


def hid(reads=10):
    """A keyboard's report descriptor over a control request to its
    interface, then interrupt IN reads: with no one typing each one times out,
    and each timeout must leave the endpoint usable for the next."""
    for dev in usb.core.find(find_all=True):
        for k, v in _walk(dev):
            if k == "ep" and v[0] and v[0][2] == 0x03 and v[2] == 3 and v[1] & 0x80:
                itf, ep = v[0][0], v[1]
                break
        else:
            continue
        report = dev.ctrl_transfer(0x81, 6, 0x2200, itf, 256, 1000)
        timeouts = got = 0
        buf = bytearray(8)
        t0 = time.ticks_ms()
        for _ in range(reads):
            try:
                got += dev.read(ep, buf, timeout=100) > 0
            except usb.core.USBTimeoutError:
                timeouts += 1
        dev._release_all()
        return {"device": "%04x:%04x" % (dev.idVendor, dev.idProduct), "interface": itf, "endpoint": hex(ep),
                "report_descriptor": len(report), "reports": got, "timeouts": timeouts,
                "ms": time.ticks_diff(time.ticks_ms(), t0)}
    return None
