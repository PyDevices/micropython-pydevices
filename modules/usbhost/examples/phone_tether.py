"""Get online through a phone: USB tethering on the board's host port.

    mpftp run -d COM55 examples/phone_tether.py
    mpftp monitor COM55 --seconds 150

``run`` starts the script and returns; ``monitor`` shows what it prints while
it waits for the phone.

Plug an Android phone into the board's host port, then turn on USB tethering
on the phone (Settings > Connections > Mobile Hotspot and Tethering > USB
tethering; the switch stays grey until the phone is plugged in). The phone
presents an RNDIS network adapter, gives the board an address by DHCP, and
shares its mobile data or Wi-Fi. This script waits for it, brings the link
up, and fetches a web page over it.

The board's own Wi-Fi is turned off first, so the page can only have come
through the phone. Leave ``WIFI_OFF = False`` to keep Wi-Fi up and send
sockets through the phone anyway with ``net.set_default()``.

The interface is ``usb.host.net.Tether``, from pydevices' ``usb-host-net``
(install it with mip). It works on an ESP32-P4's high-speed port, directly or
through a hub, and on an ESP32-S3's full-speed port. RNDIS is what Samsung
and many other Android phones speak; CDC-ECM, CDC-NCM and iPhone tethering
are not implemented.
"""

import socket
import time

import usb.core
from usb.host.net import Tether

# True turns the board's Wi-Fi off first, so the page can only come through
# the phone. False leaves Wi-Fi up and makes the phone the default route.
WIFI_OFF = True

# How long to wait for the tethering switch (a phone can take most of a
# minute to change its USB mode), and then for DHCP.
WAIT_PHONE_MS = 120000
WAIT_DHCP_MS = 30000

HOST = "example.com"


def wifi_off():
    try:
        import network
    except ImportError:
        return                      # no radio: nothing to turn off
    wlan = network.WLAN(network.STA_IF)
    try:
        wlan.disconnect()
    except OSError:
        pass
    wlan.active(False)
    print("Wi-Fi off")


def fetch(host):
    """The status line of ``http://host/``."""
    addr = socket.getaddrinfo(host, 80)[0][-1]
    s = socket.socket()
    s.settimeout(10)
    try:
        s.connect(addr)
        s.send(b"GET / HTTP/1.0\r\nHost: " + host.encode() + b"\r\n\r\n")
        return s.recv(64).split(b"\r\n")[0].decode()
    finally:
        s.close()


def main():
    if WIFI_OFF:
        wifi_off()

    usb.core.start()
    print("waiting for the phone: turn on USB tethering")
    try:
        net = Tether(wait_ms=WAIT_PHONE_MS)
    except OSError as e:
        print(e)
        usb.core.stop()
        return
    print("phone {:04x}:{:04x}".format(net.dev.idVendor, net.dev.idProduct))

    t0 = time.ticks_ms()
    net.active(True)
    while not net.isconnected():
        if time.ticks_diff(time.ticks_ms(), t0) > WAIT_DHCP_MS:
            print("no address from the phone")
            net.active(False)
            usb.core.stop()
            return
        time.sleep_ms(100)
    print("address in {} ms".format(time.ticks_diff(time.ticks_ms(), t0)))
    print("ifconfig", net.ifconfig())
    if not WIFI_OFF:
        net.set_default()

    print(HOST, "->", fetch(HOST))
    print("frames (rx, tx, rx dropped, tx dropped, rx errors)", net.stats())
    # The link stays up for your own code from here: sockets, requests and
    # mip all go through the phone. net.active(False) takes it down.


main()
