# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Check the camera when the picture is wrong or missing.

``stats()`` is the first thing to look at. If ``isr_done`` isn't climbing,
nothing is arriving, and the problem is upstream of this module: power, clock,
lanes, or the sensor not streaming. If it is climbing and ``frames`` isn't,
frames are arriving and being rejected, and ``last_received`` says how big they
were. The program shows a live scene, then the sensor's test pattern, and
prints the stats after each. The test pattern is generated inside the sensor,
after the pixel array and before everything else. A clean pattern with a broken
picture means the fault is in front of the sensor (optics, light, a lens cap).
A broken pattern too means everything after the pixel array is suspect.
``reg()`` is for a deliberate poke when you already know which register you
mean, not a tour of the sensor map.

Needs: an ESP32-P4 board with a MIPI-CSI camera and a display, both set up
in board_config, and firmware built with cameraif.

Run it:

    mpremote run camera_diag.py

Tested on: an ESP32-P4 board with an OV5647 camera.
"""

import time

import board_config
from board_config import display_drv as display


def main():
    # Report what the driver knows about the sensor.
    cam = board_config.camera
    print("sensor %s id 0x%04x" % cam.sensor())
    print("size", cam.size())
    print("formats:")
    for name, w, h in cam.formats():
        print("  ", name, w, h)
    print("controls", cam.controls())

    fb = display.framebuffers()[0]

    print("\n-- live scene, 2 s --")
    # Show the real scene first, with the test pattern off.
    cam.test_pattern(False)
    end = time.ticks_add(time.ticks_ms(), 2000)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        cam.capture_scaled(fb, display.width, display.height, timeout=500)
        display.show()
    print("stats", cam.stats())

    # Then the pattern from inside the sensor.
    print("\n-- test pattern, 2 s --")
    cam.test_pattern(True)
    end = time.ticks_add(time.ticks_ms(), 2000)
    while time.ticks_diff(end, time.ticks_ms()) > 0:
        cam.capture_scaled(fb, display.width, display.height, timeout=500)
        display.show()
    print("stats", cam.stats())
    print("test_pattern() reads back last ask:", cam.test_pattern())

    # A single register read as a smoke check that SCCB still answers.
    # 0x300A/0x300B are the OV5647 chip id registers; other sensors will
    # return something else or raise -- that is still information.
    try:
        print("reg(0x300A)=", hex(cam.reg(0x300A)),
              "reg(0x300B)=", hex(cam.reg(0x300B)))
    except Exception as exc:
        print("reg() not usable on this sensor/driver:", exc)

    cam.test_pattern(False)
    cam.deinit()
    print("done")


if __name__ == "__main__":
    main()
