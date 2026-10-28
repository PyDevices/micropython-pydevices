# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Show pictures that arrive over the network on the board's display.

The board listens on PORT for a sender. It asks for a picture by sending
one byte, and the sender answers with a 4-byte length followed by a JPEG. It decodes each one with ``jpegio.JpegDecoder``,
shrinking it by a half, a quarter or an eighth until it fits the display,
and shows it in the middle of the screen.

The sender can be another board running camera_send_simpletest.py, or a
computer sending photos: a few lines of Python that wait for each byte, then
send a file's length and the file.

Needs: a MicroPython firmware built with jpegio, on a board whose
board_config has a display_drv, already connected to Wi-Fi. CircuitPython's
own jpegio decodes into a displayio Bitmap instead, so this example is for
MicroPython.

Run it:
    mpremote run jpegio_viewer_simpletest.py

Tested on: a Waveshare ESP32-S3-Touch-LCD-4.3 on MicroPython 1.30 preview,
showing photos sent from CPython 3.14 on Windows, and pictures from an
ESP32-P4 running camera_send_simpletest.py.
"""

import socket
import struct

import jpegio
from board_config import display_drv

# Change these to match your setup.
PORT = 8080
MAX_BYTES = 200_000  # the largest picture to accept


def receive(sock, buf):
    """Fill buf from the socket. False if the sender hung up first."""
    view = memoryview(buf)
    while view:
        count = sock.readinto(view)
        if not count:
            return False
        view = view[count:]
    return True


def show(jpeg):
    """Decode one JPEG to fit the display and show it in the middle."""
    global picture
    # Shrink it until it fits: scale 1 is a half, up to 3, an eighth.
    width, height = decoder.open(jpeg)
    scale = 0
    while scale < 3 and (width >> scale > display_drv.width or height >> scale > display_drv.height):
        scale += 1
    width >>= scale
    height >>= scale

    # Decode it into a picture the same size, and put it on the screen.
    if len(picture) != width * height * 2:
        picture = bytearray(width * height * 2)
        display_drv.fill(0)
    decoder.decode(picture, scale)
    x = (display_drv.width - width) // 2
    y = (display_drv.height - height) // 2
    display_drv.blit_rect(picture, x, y, width, height)
    display_drv.show()
    return width, height


decoder = jpegio.JpegDecoder()
length = bytearray(4)
jpeg = bytearray(MAX_BYTES)
picture = bytearray(0)

# Listen for a sender.
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(socket.getaddrinfo("0.0.0.0", PORT)[0][-1])
listener.listen(1)
print("Waiting for pictures on port %d" % PORT)

# Ask for a picture, show it, and ask for the next, until the sender hangs
# up or goes quiet. Asking only when ready keeps the pictures fresh. Then
# wait for the next sender. Close the port on Ctrl-C.
try:
    while True:
        sender, peer = listener.accept()
        sender.settimeout(10)
        print("Pictures from", peer[0] if isinstance(peer, tuple) else peer)
        display_drv.fill(0)
        shown = 0
        try:
            while sender.write(b"\x01") and receive(sender, length):
                size = struct.unpack(">I", length)[0]
                if size > MAX_BYTES or not receive(sender, memoryview(jpeg)[:size]):
                    break
                shown += 1
                print("Showed picture %d at %dx%d" % ((shown,) + show(memoryview(jpeg)[:size])))
        except OSError as error:
            print("Lost the sender:", error)
        sender.close()
        print("Sender done after %d pictures" % shown)
finally:
    listener.close()
