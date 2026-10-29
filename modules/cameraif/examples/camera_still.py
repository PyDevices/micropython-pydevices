# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Preview the camera on the panel and take a photo with the BOOT button.

Point the camera at something and press the BOOT button, and a JPEG appears
on the board's filesystem. Press again for another. The board_config module
publishes the BOOT button as ``boot_button``, a plain input. This program wraps
it in ``GPIOButtons``, and ``appdev`` turns that into ordinary KEYDOWN events,
the same ones a USB keyboard would produce, so nothing here depends on which
GPIO the button is.

``appdev`` is the scheduler. It polls the inputs, dispatches the events and
keeps the program alive after the last line runs, so there is no ``while
True`` here. The preview runs on ``app.every()`` because repainting is
periodic work, and the shutter on ``app.on()`` because it happens when it
happens. Anything drawn must be followed by ``display_drv.show()``, which puts
the back buffer on the glass. The shutter handler only sets a flag, and the
next tick does the work, so all camera use stays in one place.

Hold the button for a moment: a preview frame costs about 54 ms, so the
button is read a handful of times a second, and a very quick tap can fall
between two reads. Ctrl-C can't stop the program, because ``appdev`` drives it
from a hardware timer. Press the board's reset button instead.

Needs: an ESP32-P4 board with a MIPI-CSI camera, a display and a BOOT
button, set up in board_config, firmware built with cameraif, and the
``appdev`` and ``keypad_gpio`` modules.

Run it, then press BOOT:

    mpremote run camera_still.py
    mpremote ls
    mpremote cp :photo_001.jpg .
"""

import appdev
import board_config
import keys
from board_config import display_drv as display
from keypad_gpio import GPIOButtons

# Change these to match your setup.
QUALITY = 90  # 1 to 100; higher is sharper and larger
# The preview costs about 54 ms a frame, so a period below that would eat the
# scheduler the input polling shares.
PREVIEW_MS = 100  # milliseconds between preview frames

app = appdev.App(board_config)
cam = board_config.camera

# BOOT as a key: the board hands over the raw input, and this app decides it
# is the shutter.
shutter_button = GPIOButtons({"shutter": (board_config.boot_button, keys.K_LCTRL)})
app.add_keypad(shutter_button.read)

# The panel's own scanout buffer. capture_scaled() scales the camera frame
# into it with the PPA, by DMA, so the live view costs the CPU one call.
framebuffer = display.framebuffers()[0]

# State shared by the button handler and the tick.
count = 0
shot_requested = False


def next_filename():
    """First unused photo_NNN.jpg.

    Deliberately not a counter starting at 1 each run: a board that reboots
    would silently overwrite yesterday's pictures, and that failure only
    surfaces when someone goes looking for one.
    """
    import os

    try:
        existing = set(os.listdir())
    except OSError:
        existing = set()
    n = 1
    while ("photo_%03d.jpg" % n) in existing:
        n += 1
    return "photo_%03d.jpg" % n


def shutter(_event):
    """The button. Records the request; the next tick does the work."""
    global shot_requested
    shot_requested = True


def take_photo():
    global count
    # A white frame, before the capture rather than after: it is the only
    # feedback there is on a board with no shutter sound, and it should mark
    # the moment the picture was taken.
    display.fill(0xFFFF)
    display.show()
    try:
        img = cam.capture_jpeg(QUALITY)
        if img is None:
            print("shutter: no frame")
            return
        # A truncated encode still returns bytes. A file that is the right
        # size but has no end-of-image marker fails much later, on somebody
        # else's computer.
        if img[:2] != b"\xff\xd8" or img[-2:] != b"\xff\xd9":
            print("shutter: encoder returned %d bytes that are not a JPEG" % len(img))
            return
        name = next_filename()
        with open(name, "wb") as f:
            f.write(img)
        count += 1
        print("%s  %d bytes  (%d this session)" % (name, len(img), count))
    finally:
        display.fill(0)


def tick(_=None):
    """The one place camera work happens: a photo, or a preview frame."""
    global shot_requested
    if shot_requested:
        shot_requested = False
        take_photo()
    elif cam.capture_scaled(framebuffer, display.width, display.height,
                            timeout=200) is not None:
        display.show()


# Run the shutter handler on a key press and the tick on a timer.
app.on(app.events.KEYDOWN, shutter)
app.every(PREVIEW_MS, tick)

print("Live view up. Press BOOT to take a picture.")
