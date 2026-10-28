# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Find a web page on the network by its name, and read it.

The board asks the network for ``_http._tcp`` services with mDNS, picks the
one called INSTANCE_NAME, and fetches its page from the address and port
it answered with. No address is written down anywhere.

The page can be another board, or a computer, running
mdns_advertise_simpletest.py, or anything else that advertises
``_http._tcp``: a printer's setup page, say. Set INSTANCE_NAME to the name
it goes by, or leave it empty to list them all.

Needs: a network. On a board, CircuitPython, or a MicroPython firmware
built with mdns, already connected to Wi-Fi. On a computer, the MicroPython
unix or Windows port built with mdns, or CPython with pydevices-desktop and
python-zeroconf.

Run it:
    mpremote run mdns_find_simpletest.py

On CircuitPython, copy it to the board as code.py.

Tested on: ESP32-S3 boards on MicroPython 1.30 preview and CircuitPython
11.0 alpha, each finding the other running mdns_advertise_simpletest.py.
"""

import mdns

# Change these to match your setup.
INSTANCE_NAME = "PyDevices page"  # "" lists every web page and stops
SEARCH_SECONDS = 3

# On a board, use the Wi-Fi interface: network.WLAN on MicroPython,
# wifi.radio on CircuitPython. On a computer, use None.
try:
    import network

    interface = network.WLAN(network.STA_IF)
except (ImportError, AttributeError):
    try:
        import wifi

        interface = wifi.radio
    except ImportError:
        interface = None

# CircuitPython makes sockets from a pool on the radio; elsewhere there's
# socket.
try:
    import socketpool

    pool = socketpool.SocketPool(interface)
except ImportError:
    import socket as pool


def receive(sock, buf):
    """Read what has arrived into buf, and return how many bytes.

    CircuitPython's sockets read with recv_into, MicroPython's with recv.
    """
    if hasattr(sock, "recv_into"):
        return sock.recv_into(buf)
    data = sock.recv(len(buf))
    buf[: len(data)] = data
    return len(data)


# Ask the network for every web service, and pick ours by name.
server = mdns.Server(interface)
found = None
for service in server.find(service_type="_http", protocol="_tcp", timeout=SEARCH_SECONDS):
    print("Heard %r at %s, port %d" % (service.instance_name, service.ipv4_address, service.port))
    if service.instance_name == INSTANCE_NAME:
        found = service
server.deinit()

# Fetch its page from the address it answered with.
if found is None:
    print("No page called %r answered" % INSTANCE_NAME)
else:
    s = pool.socket()
    s.connect(pool.getaddrinfo(str(found.ipv4_address), found.port)[0][-1])
    s.send(b"GET / HTTP/1.0\r\n\r\n")

    # Read until the page closes the connection.
    chunk = bytearray(512)
    reply = b""
    while True:
        n = receive(s, chunk)
        if not n:
            break
        reply += chunk[:n]
    s.close()
    print("The page says:", reply.split(b"\r\n\r\n", 1)[-1].decode().strip())
