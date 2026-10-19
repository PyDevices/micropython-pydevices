"""List the USB devices on the host port and watch attach / detach.

Uses ``usb.core``, whose ``find()`` is PyUSB's and CircuitPython's, so what
you learn here carries over to any class driver written on it. Each device
is printed with its vendor and product IDs, speed, and the manufacturer,
product and serial strings it reports (any it lacks print as ``-``).

    mpftp run -d COM49 examples/host_enum.py

Plug and unplug devices while it runs; each attach and detach is printed,
along with the host's counters from ``usb.core.stats()``. On an S3 host that
means a powered hub or OTG adapter (no VBUS switching on the Waveshare touch
boards). A P4 hosts on a port that supplies 5 V. Behind a hub it sees full-
and low-speed devices only when the host runs at full speed: set
``FULL_SPEED = True`` below.
"""

import time

import usb.core

# True when devices are behind a hub on an ESP32-P4: the host then runs at
# full speed, so the hub passes full- and low-speed devices through.
FULL_SPEED = False

_SPEEDS = {1: "low", 2: "full", 3: "high"}


def describe(dev):
    try:
        names = [dev.manufacturer, dev.product, dev.serial_number]
    except usb.core.USBError as exc:
        names = ["?", "strings unreadable: %s" % exc, "?"]
    return "%04x:%04x %-4s  %s / %s / %s" % (
        dev.idVendor, dev.idProduct, _SPEEDS.get(dev.speed, "?"),
        names[0] or "-", names[1] or "-", names[2] or "-")


def listing():
    return [describe(dev) for dev in usb.core.find(find_all=True)]


def counters(stats):
    return (stats["attaches"], stats["detaches"]) if stats else None


def main(seconds=60):
    usb.core.start(full_speed=FULL_SPEED)
    try:
        now = listing()
        print("currently attached:")
        for line in now:
            print(" ", line)
        if not now:
            print("  (none yet -- plug something in)")
        stats = usb.core.stats()
        print("host:", stats)

        print("watching attach/detach for %d s ..." % seconds)
        deadline = time.ticks_add(time.ticks_ms(), seconds * 1000)
        while time.ticks_diff(deadline, time.ticks_ms()) > 0:
            time.sleep_ms(250)
            st = usb.core.stats()
            # List again only when the host counted an attach or detach:
            # each listing reads every device's strings over its control pipe.
            if counters(st) == counters(stats):
                continue
            stats = st
            time.sleep_ms(500)      # a device just attached is still enumerating
            seen = listing()
            # Two of the same make count as two: match them off one by one.
            added = list(seen)
            for line in now:
                if line in added:
                    added.remove(line)
                else:
                    print("detach:", line)
            for line in added:
                print("attach:", line)
            now = seen
            print("host:", st)
    except KeyboardInterrupt:
        print("stopped")
    finally:
        usb.core.stop()


if __name__ == "__main__":
    main()
