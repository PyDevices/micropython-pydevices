# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Serve a web page and announce it by name, so nobody needs the address.

The board serves a one-line web page and advertises it with mDNS as a
``_http._tcp`` service under INSTANCE_NAME. Anything on the network can
then find it by that name, the way a phone finds a printer, and the board's
address can change without anyone noticing.

The finder can be another board, or a computer, running
mdns_find_simpletest.py. A Mac or iPhone sees it in Safari's Bonjour list,
and any computer can open http://<hostname>.local/ by the hostname it
prints.

Needs: a network. On a board, CircuitPython, or a MicroPython firmware
built with mdns, already connected to Wi-Fi. On a computer, the MicroPython
unix or Windows port built with mdns, or CPython with pydevices-desktop and
python-zeroconf.

Run it:
    mpremote run mdns_advertise_simpletest.py

On CircuitPython, copy it to the board as code.py.

Tested on: ESP32-S3 boards on MicroPython 1.30 preview and CircuitPython
11.0 alpha, each found by the other running mdns_find_simpletest.py.
"""

import mdns

# Change these to match your setup.
INSTANCE_NAME = "PyDevices page"
PORT = 8080
VISITS = 3

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


# Listen for web browsers first, so the service works once it's announced.
listener = pool.socket()
listener.setsockopt(pool.SOL_SOCKET, pool.SO_REUSEADDR, 1)
listener.bind(pool.getaddrinfo("0.0.0.0", PORT)[0][-1])
listener.listen(1)

# Announce the page by name.
server = mdns.Server(interface)
server.instance_name = INSTANCE_NAME
server.advertise_service(service_type="_http", protocol="_tcp", port=PORT)
print("Advertising %r as %s.local, port %d" % (INSTANCE_NAME, server.hostname, PORT))

# Answer each visit with a greeting. Then withdraw the announcement and
# close the port, even on Ctrl-C: on CircuitPython they outlive the
# program otherwise.
try:
    request = bytearray(1024)
    for _ in range(VISITS):
        client, _ = listener.accept()
        client.settimeout(5)  # wait for the request; CircuitPython's may not
        receive(client, request)  # the request; any page gets the same answer
        client.send(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n")
        client.send(b"Hello from " + INSTANCE_NAME.encode() + b"\n")
        client.close()
        print("Served a visit")
finally:
    server.deinit()
    listener.close()
print("Done")
