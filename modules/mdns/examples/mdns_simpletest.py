# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Advertise a web service on the local network, then look for others.

Announces a service of type ``_http._tcp`` under the name in
``INSTANCE_NAME``, the way a board with a web page would, then asks the
network for every ``_http._tcp`` service and prints what answers. The
service this program advertises is in the list too.

Needs: a network. On a board, a firmware built with mdns, already
connected to Wi-Fi. On a computer, the MicroPython unix or Windows port
built with mdns, or CPython with pydevices-desktop and python-zeroconf.

Run it:

    mpremote run mdns_simpletest.py

Tested on: the MicroPython unix port, built with ``--modules all``, and
CPython with pydevices-desktop on Linux and Windows.
"""

import mdns

# Change these to match your setup.
INSTANCE_NAME = "PyDevices example"
PORT = 8080
SEARCH_SECONDS = 3

# On a board, use the Wi-Fi interface. On a computer, use None.
try:
    import network

    interface = network.WLAN(network.STA_IF)
except (ImportError, AttributeError):
    interface = None

# Start mDNS and advertise the service.
server = mdns.Server(interface)
server.instance_name = INSTANCE_NAME
server.advertise_service(service_type="_http", protocol="_tcp", port=PORT, txt_records=["path=/"])
print("Advertising %r as %s.local, port %d" % (INSTANCE_NAME, server.hostname, PORT))

# Ask the network for every web service, and print each answer.
print("Looking for _http._tcp services for %d seconds..." % SEARCH_SECONDS)
for service in server.find(service_type="_http", protocol="_tcp", timeout=SEARCH_SECONDS):
    print("  %s at %s (%s), port %d" % (service.instance_name, service.hostname, service.ipv4_address, service.port))

# Withdraw the service before the program ends.
server.deinit()
