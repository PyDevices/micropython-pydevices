# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""List what the camera can be told to do, then try each control.

Sensor drivers differ in how much of the hardware they expose, so this
program asks first. ``cam.controls()`` returns only the parameters the driver
in your firmware advertises, with the range it gives for each, and
everything else is driven from that. Copy this shape rather than hard-coding
a list: code that assumes ``gain()`` exists raises OSError on the next sensor.
The OV5647's driver, for one, offers exactly three parameters and can't read
any of them back, so the program shows those as ``None``. At the end the
sensor's test pattern runs, which is the most useful debugging tool here.
Success is the picture changing on the panel as each control moves.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif.

Run it:

    mpremote run camera_controls.py

Tested on: an ESP32-P4 board with an OV5647 camera.
"""

import time

import board_config
from board_config import display_drv as display

# Open the camera and get the panel's framebuffer.
cam = board_config.camera
_fb = display.framebuffers()[0]


def show(seconds=2):
    """Paint what the camera sees, for a while."""
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        if cam.capture_scaled(_fb, display.width, display.height,
                              timeout=500) is None:
            continue
        display.show()


def main():
    print("sensor %s, id 0x%04x" % cam.sensor())
    print("formats:")
    for name, w, h in cam.formats():
        print("   %-48s %dx%d" % (name, w, h))

    # Ask the driver which controls it has, and list them with their ranges.
    controls = cam.controls()
    print("\ncontrols this driver implements:")
    for name in sorted(controls):
        lo, hi, step = controls[name]
        print("   %-12s %d..%d step %d" % (name, lo, hi, step))
    missing = {"exposure", "gain", "flip", "mirror"} - set(controls)
    if missing:
        print("   (not offered by this driver: %s)" % ", ".join(sorted(missing)))

    try:
        # Percent of the advertised range, not raw units. The units are not
        # portable -- exposure is in lines, gain in driver-specific steps --
        # and both ranges move with the format, so a literal that suits one
        # mode is quietly wrong in another. Pass raw=True when you do mean
        # the sensor's own numbers.
        for name in ("exposure", "gain"):
            if name not in controls:
                continue
            print("\n%s sweep" % name)
            for pct in (10, 40, 70, 100):
                getattr(cam, name)(pct)
                print("   %s %d%%" % (name, pct))
                show(1.5)
            getattr(cam, name)(50)

        if "flip" in controls or "mirror" in controls:
            print("\norientation")
            for flip, mirror, label in ((False, False, "normal"),
                                        (True, False, "flipped (upside down)"),
                                        (False, True, "mirrored (as in a mirror)"),
                                        (True, True, "both (180 degrees)")):
                if "flip" in controls:
                    cam.flip(flip)
                if "mirror" in controls:
                    cam.mirror(mirror)
                print("   %s" % label)
                show(2)
            if "flip" in controls:
                cam.flip(False)
            if "mirror" in controls:
                cam.mirror(False)

        # Reading back is a separate question from setting. This driver
        # cannot, and says so by returning None rather than a plausible zero.
        print("\nread-back:")
        for name in sorted(controls):
            print("   %-12s -> %r" % (name, getattr(cam, name)()))

        print("\nsensor test pattern")
        # Generated inside the sensor, after the pixel array and before
        # everything else: the MIPI lanes, the ISP, the DMA, the cache
        # invalidate, your blit. Clean pattern with a bad picture means the
        # fault is in front of the sensor -- optics, focus, light, a lens
        # cap. Broken pattern means everything after the pixel array is
        # suspect. Either answer halves the search.
        try:
            cam.test_pattern(True)
            show(4)
            cam.test_pattern(False)
            show(2)
        except OSError as e:
            print("   not available: %s" % e)

        # Registers, for when the datasheet and the picture disagree.
        # 0x300a/0x300b is the OV5647's product ID.
        print("\nchip id from registers: 0x%02x%02x" % (cam.reg(0x300A), cam.reg(0x300B)))
        print("frames/dropped/isr_new/isr_done/last:", cam.stats())
    finally:
        cam.deinit()


if __name__ == "__main__":
    main()
