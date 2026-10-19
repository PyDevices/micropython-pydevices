"""What Windows was told: a USB device's speed and its endpoints, from the hub.

    python usb_desc.py VID [SERIAL]       e.g.  python usb_desc.py 303A E8F60AE0F070

Asks every USB hub (as USBView does, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX
and IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION) for the devices on its
ports, and for each one with that vendor ID (and serial number, when given)
prints a JSON line: the connection speed, and every endpoint of the active
configuration with its type, wMaxPacketSize and bInterval. This is the
descriptor the computer actually received, not what the board meant to send.
"""
import ctypes
import json
import sys
from ctypes import wintypes as w

setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

GUID_USB_HUB = "{f18a0e88-c30c-11d0-8815-00a0c906bed8}"
DIGCF_PRESENT, DIGCF_DEVICEINTERFACE = 0x2, 0x10
IOCTL_NODE_INFO = 0x220408
IOCTL_CONN_INFO_EX = 0x220448
IOCTL_DESC_FROM_CONN = 0x220410
SPEEDS = {0: "low", 1: "full", 2: "high", 3: "super"}
TYPES = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}


class GUID(ctypes.Structure):
    _fields_ = [("Data1", w.DWORD), ("Data2", w.WORD), ("Data3", w.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("InterfaceClassGuid", GUID), ("Flags", w.DWORD), ("Reserved", ctypes.c_void_p)]


ole32 = ctypes.WinDLL("ole32")
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), w.DWORD,
                                                 ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
                                                      ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), ctypes.c_void_p]
kernel32.CreateFileW.restype = w.HANDLE
kernel32.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, ctypes.c_void_p, w.DWORD, ctypes.c_void_p, w.DWORD,
                                     ctypes.POINTER(w.DWORD), ctypes.c_void_p]


def hub_paths():
    guid = GUID()
    ole32.CLSIDFromString(GUID_USB_HUB, ctypes.byref(guid))
    info = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    i = 0
    while True:
        did = SP_DEVICE_INTERFACE_DATA(cbSize=ctypes.sizeof(SP_DEVICE_INTERFACE_DATA))
        if not setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), i, ctypes.byref(did)):
            return
        need = w.DWORD(0)
        setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(did), None, 0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(w.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        if setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(did), buf, need, None, None):
            yield ctypes.wstring_at(ctypes.addressof(buf) + 4)
        i += 1


def ioctl(h, code, inbuf, outsize):
    out = ctypes.create_string_buffer(outsize)
    got = w.DWORD(0)
    ok = kernel32.DeviceIoControl(h, code, inbuf, len(inbuf), out, outsize, ctypes.byref(got), None)
    return out.raw[: got.value] if ok else None


def descriptor(h, port, dtype, index, length):
    # USB_DESCRIPTOR_REQUEST: ConnectionIndex, then a SETUP packet, then data.
    req = (port.to_bytes(4, "little") + bytes([0x80, 6]) + ((dtype << 8) | index).to_bytes(2, "little")
           + (0x0409 if dtype == 3 else 0).to_bytes(2, "little") + length.to_bytes(2, "little")
           + bytes(length))
    out = ioctl(h, IOCTL_DESC_FROM_CONN, req, len(req))
    return out[12:] if out else None


def main():
    vid = int(sys.argv[1], 16)
    serial = sys.argv[2].upper() if len(sys.argv) > 2 else None
    found = 0
    for path in hub_paths():
        h = kernel32.CreateFileW(path, 0x40000000, 2, None, 3, 0, None)
        if h in (None, w.HANDLE(-1).value):
            continue
        node = ioctl(h, IOCTL_NODE_INFO, bytes(76), 76)
        ports = node[6] if node else 0
        for port in range(1, ports + 1):
            info = ioctl(h, IOCTL_CONN_INFO_EX, port.to_bytes(4, "little") + bytes(64), 68 + 512)
            if not info or len(info) < 30:
                continue
            dd = info[4:22]
            if int.from_bytes(dd[8:10], "little") != vid:
                continue
            speed = SPEEDS.get(info[23], str(info[23]))
            iserial = dd[16]
            sn = ""
            if iserial:
                s = descriptor(h, port, 3, iserial, 255)
                sn = s[2 : s[0]].decode("utf-16-le", "replace").upper() if s else ""
            if serial and serial not in sn:
                continue
            head = descriptor(h, port, 2, 0, 9)
            total = int.from_bytes(head[2:4], "little") if head else 0
            cfg = descriptor(h, port, 2, 0, total) if total else b""
            eps, i = [], 0
            while i + 2 <= len(cfg) and cfg[i] >= 2:
                if cfg[i + 1] == 5 and cfg[i] >= 7:
                    eps.append({"address": "0x%02x" % cfg[i + 2], "type": TYPES[cfg[i + 3] & 3],
                                "max_packet": int.from_bytes(cfg[i + 4 : i + 6], "little"), "interval": cfg[i + 6]})
                i += cfg[i]
            print(json.dumps({"serial": sn, "pid": "%04x" % int.from_bytes(dd[10:12], "little"),
                              "speed": speed, "endpoints": eps, "config": cfg.hex()}))
            found += 1
        kernel32.CloseHandle(h)
    if not found:
        print(json.dumps({"error": "no device with that vendor and serial"}))
        sys.exit(1)


main()
