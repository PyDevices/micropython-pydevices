# The board half of the device-class proof matrix (tools/matrix/run_matrix.py).
#
# setup(names) brings up any combination of "cdc" (an extra serial port),
# "hid", "midi" and "msc" on machine.USBDevice, beside the board's own serial
# port; serve(seconds) then echoes MIDI and the extra serial port back to the
# computer while the runner's check runs. The runner drives it over WebREPL, because the board's USB
# serial port re-enumerates with every setup().
import os
import time

import machine
import usb.device

state = {}
_buf = bytearray(256)
_drop = None  # the runner's --plant sets this to 0: drop every 50th MIDI read


def _pump():
    global _drop
    m = state.get("midi")
    if m is not None:
        n = m.ports[0].readinto(_buf)
        if n:
            if _drop is not None:
                _drop += 1
                if _drop % 50 == 0:
                    n = 0
            if n:
                m.ports[1].write(_buf, n)
    c = state.get("cdc")
    if c is not None and c.is_open():
        n = c.readinto(_buf)
        if n:
            c.write(_buf[:n])


def setup(names):
    """Bring up these functions; returns the error text or None."""
    stop()
    itfs = []
    if "msc" in names:
        from usb.device.msc import MSCInterface, RAMBlockDev

        bd = RAMBlockDev(256)
        os.VfsFat.mkfs(bd)
        os.mount(os.VfsFat(bd), "/mx")
        with open("/mx/hello.txt", "w") as f:
            f.write("matrix\n")
        os.umount("/mx")
        state["msc"] = MSCInterface(bd, product="matrix drive")
        itfs.append(state["msc"])
    if "hid" in names:
        import usb_hid

        itfs.append(usb_hid.interface())
        state["hid"] = usb_hid
    if "midi" in names:
        from midiports.usb_device import MIDIPortsInterface

        state["midi"] = MIDIPortsInterface(ring=2048)
        itfs.append(state["midi"])
    if "cdc" in names:
        from usb.device.cdc import CDCInterface

        state["cdc"] = CDCInterface(timeout=0)
        itfs.append(state["cdc"])
    try:
        usb.device.get().init(*itfs, builtin_driver=True)
    except ValueError as e:
        state.clear()
        return str(e)
    return None


def serve(seconds):
    """Echo MIDI and the extra serial port back for this long. A foreground
    loop, not a timer: USB transfer callbacks run from MicroPython's
    scheduler, and a fast timer's callbacks crowd them out of its queue."""
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        _pump()
        time.sleep_ms(1)
    return True


def stop():
    state.clear()


def hid_burst(n):
    """n mouse reports, one pixel right then left, so the pointer stays put."""
    from adafruit_hid.mouse import Mouse

    mouse = Mouse(state["hid"].devices)
    for i in range(n):
        mouse.move(x=1 if i % 2 == 0 else -1)
        time.sleep_ms(4)
    return n


def msc_read(name):
    """A file the computer wrote to the drive, read on the board."""
    bd = state["msc"].bdev
    os.mount(os.VfsFat(bd), "/mx", readonly=True)
    try:
        with open("/mx/" + name) as f:
            return f.read()
    finally:
        os.umount("/mx")


def speed():
    return machine.USBDevice().speed()
