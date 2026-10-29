# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Use CircuitPython's espcamera API on a MIPI-CSI camera.

On an ESP32-P4, cameraif is also importable as ``espcamera``, and it answers
to espcamera's names: ``take()``, ``pixel_format``, ``frame_size``,
``reconfigure()``, and the controls as properties (``vflip``, ``hmirror``,
``aec_value``, ``colorbar``). Code written for espcamera runs here with one
change, the constructor: espcamera's takes DVP data pins, and this camera is on
the CSI connector. The program shows a live view on the panel, turns the
picture over and back with ``vflip`` and ``hmirror``, and saves one JPEG.
A control the sensor lacks raises ``AttributeError``, as in espcamera, so
``hasattr(cam, "agc_gain")`` is how to ask. ``cam.controls()`` lists the ones
the sensor's driver has.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif.

Run it:

    mpremote run camera_espcamera.py
    mpremote cp :espcamera.jpg .

Tested on: an ESP32-P4 board with an IMX219 camera and a 720x720 panel, on
MicroPython v1.30.0-preview.160.gb70ae1aca6 (built 2026-10-28).
"""

import time

import board_config
import espcamera
from board_config import display_drv as display

# The one line that differs from espcamera code: the board's camera, which
# is a cameraif.Camera on its CSI pins. On your own board this is
# espcamera.Camera(sda, scl, i2c=..., frame_size=..., pixel_format=...).
cam = board_config.camera

cam.reconfigure(pixel_format=espcamera.PixelFormat.RGB565,
                frame_size=espcamera.FrameSize.HVGA)
print("%s at 0x%02x, taking %dx%d" % (cam.sensor_name, cam.address, cam.width, cam.height))

# Centre the picture on the panel.
x = (display.width - cam.width) // 2
y = (display.height - cam.height) // 2


def view(seconds, label):
    """Show what take() returns, centred on the panel, for a while."""
    print(label)
    n = 0
    t0 = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), t0) < seconds * 1000:
        frame = cam.take(1)
        if frame is None:
            continue    # a missed frame is timing, not failure
        display.blit_rect(frame, x, y, cam.width, cam.height)
        display.show()
        n += 1
    print("  %.1f fps" % (n * 1000 / time.ticks_diff(time.ticks_ms(), t0)))


# Show the picture upright, then upside down, then mirrored.
view(3, "upright")
cam.vflip = True
view(3, "vflip: upside down")
cam.vflip = False
cam.hmirror = True
view(3, "hmirror: as in a mirror")
cam.hmirror = False

# Report which controls this sensor has.
for name in ("vflip", "hmirror", "aec_value", "agc_gain", "brightness"):
    print("%-10s %s" % (name, "yes" if hasattr(cam, name) else "not on this sensor"))

# A still: switch take() to JPEG at a bigger size, take one, switch back.
cam.reconfigure(pixel_format=espcamera.PixelFormat.JPEG, frame_size=espcamera.FrameSize.SVGA)
jpeg = cam.take(1)
if isinstance(jpeg, memoryview):
    with open("espcamera.jpg", "wb") as f:
        f.write(jpeg)
    print("espcamera.jpg: %d bytes, %dx%d" % (len(jpeg), cam.width, cam.height))
cam.reconfigure(pixel_format=espcamera.PixelFormat.RGB565, frame_size=espcamera.FrameSize.HVGA)
