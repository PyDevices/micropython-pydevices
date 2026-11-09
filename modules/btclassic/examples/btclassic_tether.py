# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Reach the internet through a phone's Bluetooth tethering.

The board joins a phone's Bluetooth tethering (PAN) and gets an address
from it, as it would from a Wi-Fi network. Once it's up, sockets go
through the phone: the board prints its address and fetches a web page,
then prints the link's counters.

Needs: a Pico W or Pico 2 W with firmware built with the btclassic module
(an ESP32's Bluetooth stack has no PAN), and a phone paired with the
board with Bluetooth tethering turned on in its hotspot settings.

Run it:

    mpftp run btclassic_tether.py

Tested on:
"""

import socket

import btclassic

# Change these to match your setup.
ADDRESS = None  # the phone's 6-byte address, or None for the last device
HOST = "micropython.org"  # the web server to fetch from

# Join the phone's tethering. This waits until the phone gives the board
# an address.
bt = btclassic.Bluetooth()
tether = btclassic.Tether(bt)
tether.connect(ADDRESS)
address, netmask, gateway, dns = tether.ifconfig()
print("Tethered: address %s, gateway %s" % (address, gateway))

# Fetch the page's headers over plain HTTP; the tether is the default route.
info = socket.getaddrinfo(HOST, 80)[0][-1]
sock = socket.socket()
sock.connect(info)
sock.send(b"HEAD / HTTP/1.0\r\nHost: %s\r\n\r\n" % HOST.encode())
print(sock.recv(200).split(b"\r\n")[0].decode())
sock.close()

# Print the counters and leave the tethering.
print(tether.stats())
tether.deinit()
