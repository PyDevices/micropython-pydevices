# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Show a USB webcam on the board's own display.

The board is the USB host. ``usb.host.video.Camera`` reads a UVC camera's
descriptors, negotiates a video mode with it and streams frames over
isochronous transfers, and this script draws them on the display that
``board_config`` set up. MJPEG is preferred when the firmware has ``jpegio``,
because a webcam offers far better resolutions in MJPEG than uncompressed;
without it the example uses uncompressed YUY2. ``uvc_display_helpers.py``
holds the camera search, the mode negotiation and the frame drawing.

Needs: a board with a display and USB host support (firmware built with
usbhost), a UVC camera on the host port, and ``uvc_display_helpers.py`` copied
to the board next to this file. A P4 can host on a port that supplies 5 V,
such as the ESP32-P4-WIFI6-DEV-KIT's Type-A socket.

Run it:

    mpftp put examples/uvc_display_helpers.py /uvc_display_helpers.py
    mpftp run examples/uvc_display.py

Success is the camera's picture on the display and a line every 25 frames
giving the frame rate.
"""

import appdev
import board_config
from board_config import display_drv
from uvc_display_helpers import find_camera, make_tick, open_camera, scale_geometry

import usb.core

try:
    import jpegio
except ImportError:
    jpegio = None

# Change these to match your setup.
FPS = 5  # frame rate to ask for; None asks for the highest that fits
PREFER_MJPEG = True  # False uses uncompressed YUY2 even when jpegio is present
MAX_SIZE = None  # largest (width, height) to ask for; None means what fits the display
UPSCALE = None  # whole-number enlargement; None means as large as fits
TEAR_FREE = True  # swap buffers on the panel's refresh; False is faster but can show a seam
BACKGROUND = True  # run on the app's 10 ms timer, keeping the REPL usable
# The camera sends one packet per bus frame, so a mode is reachable only if its
# payload fits the host's isochronous IN limit. A session started for "uvc"
# gets the IN-leaning FIFO split, 600 bytes.
IN_LIMIT = 600

app = appdev.App(board_config)
use_mjpeg = PREFER_MJPEG and jpegio is not None

# Start the host and wait for a camera to enumerate.
usb.core.start(classes=("uvc",))
dev = find_camera()
camera = None
if dev is None:
    print("no UVC camera found on the host port")
else:
    print("camera is", dev, dev.manufacturer, dev.product)
    if use_mjpeg:
        print("jpegio present -- preferring MJPEG")
    else:
        print("uncompressed YUY2 only (no jpegio, or PREFER_MJPEG is False)")
    size = MAX_SIZE or (display_drv.width, display_drv.height)
    camera = open_camera(dev, size, use_mjpeg, FPS, IN_LIMIT)

if camera is not None:
    print("streaming %s %dx%d @ %s fps, up to %d bytes a frame"
          % (camera.encoding, camera.width, camera.height, camera.fps, camera.frame_bytes))
    print("payload %d B/frame" % camera.payload)

    # Centre the picture on the display at the largest whole-number enlargement.
    geometry = scale_geometry(camera, display_drv, UPSCALE)
    scale, out_w, out_h, x0, y0 = geometry
    print("%dx%d upscaled x%d -> %dx%d at (%d, %d)"
          % (camera.width, camera.height, scale, out_w, out_h, x0, y0))

    camera.start()
    # This loop presents its own frames and reads no touch, so stop the app's
    # refresh timer and touch polling, which would take a share of every frame.
    refresh_claim = app.pause_refresh()
    polling_claim = app.pause_polling()
    # Black behind the picture, in both of a double-buffered panel's buffers.
    for _ in range(2):
        display_drv.fill(0)
        display_drv.show()

    # Frames arrive every 200 ms at 5 fps, so nearly every 10 ms tick returns
    # at once. Scheduling it, rather than looping, leaves touch and the REPL a turn.
    tick = make_tick(camera, display_drv, jpegio, geometry, TEAR_FREE)
    if BACKGROUND:
        app.every(tick, period=10)
    else:
        try:
            while True:
                tick()
        finally:
            camera.stop()
