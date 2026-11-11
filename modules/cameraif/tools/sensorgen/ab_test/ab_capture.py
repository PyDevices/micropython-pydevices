# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Record how the OV2710 behaves on an ESP32-P4X-EYE, for an A/B of driver against description.

Run it once per arm, with the camera on a tripod and the room lit and
unchanged between runs:

    driver       firmware with Espressif's C driver; opens with no sensor=
    description  the same firmware, with sensors/ov2710.py copied to the board
    frozen       firmware with the C driver left out and ov2710.py frozen in

For each mode it opens the camera cold and logs, every frame for SETTLE_MS,
the mean brightness of a small scaled copy, cameraif's auto-exposure state and
the sensor's own exposure and gain registers. Then it lights the flash LED, a
step in the scene's brightness, and logs the same again while exposure follows
it. Last, with the LED off and exposure settled, it saves one JPEG at full size,
one 320x180 RGB565 copy, and the value of every register the description's mode
writes, so the arms can be compared register by register.

Each run writes <OUT>/<arm>/<mode>.json, .jpg and .rgb565. Copy the folders to
the desktop and run ab_compare.py on them.

    mpftp put ab_capture.py /ab_capture.py
    mpftp put ../../../sensors/ov2710.py /ov2710.py      (description arm only)
    mpftp exec "import ab_capture; ab_capture.run('driver')"

Needs: firmware with cameraif, ppa and jpegio, and the board's config package.
"""

import json
import os
import time

import cameraif
from machine import Pin

# Where the runs go: the SD card when one is in, else the flash.
OUT = "/sd/ab"
# The modes to compare, as cameraif takes them.
MODES = ((1920, 1080), (1280, 720))
# How long each phase logs, in milliseconds.
SETTLE_MS = 6000
# The flash LED's pin, high on: the step in light the second phase follows.
LED_PIN = 23
# The OV2710's exposure, gain and AEC control registers, logged every frame.
AE_REGS = (0x3500, 0x3501, 0x3502, 0x350A, 0x350B, 0x3503)
THUMB_W = 320
THUMB_H = 180


def _open(arm, mode):
    """Open the camera the way the arm says."""
    kwargs = {"format": mode}
    if arm != "driver":
        from ov2710 import SENSOR

        kwargs["sensor"] = SENSOR
    try:
        from board_peripherals import camera
    except ImportError:
        # The EYE's own wiring, for a board without its config package.
        Pin(12, Pin.OUT, value=1)
        time.sleep_ms(10)
        return cameraif.Camera(14, 13, i2c=1, reset=26, xclk=11, **kwargs)
    return camera(**kwargs)


def _luma(cam, thumb):
    """Mean brightness, 0 to 255, of a scaled copy of the next frame."""
    if cam.capture_scaled(thumb, THUMB_W, THUMB_H) is None:
        return None
    total = 0
    # Every 7th pixel is plenty for a mean and keeps a frame's sum short.
    for i in range(0, len(thumb), 14):
        v = thumb[i] | (thumb[i + 1] << 8)
        total += ((v >> 11) << 3) * 77 + (((v >> 5) & 0x3F) << 2) * 150 + ((v & 0x1F) << 3) * 29
    return total / (256 * (len(thumb) // 14))


def _log(cam, thumb, ms):
    """Log every frame for `ms` milliseconds, as rows of numbers."""
    rows = []
    start = time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(), start) < ms:
        y = _luma(cam, thumb)
        on, target, brightness = cam.auto_exposure()
        regs = [cam.reg(r) for r in AE_REGS]
        rows.append([time.ticks_diff(time.ticks_ms(), start), y, brightness, target] + regs)
    return rows


def _mode_regs():
    """The registers the description's modes write, in order, each once."""
    from ov2710 import SENSOR

    seen = []
    for mode in SENSOR["modes"]:
        for reg, _ in mode["regs"]:
            if reg is not None and reg not in seen:
                seen.append(reg)
    return seen


def _mkdirs(path):
    part = ""
    for name in path.strip("/").split("/"):
        part += "/" + name
        try:
            os.mkdir(part)
        except OSError:
            pass


def run(arm):
    """Record every mode for one arm: "driver", "description" or "frozen"."""
    led = Pin(LED_PIN, Pin.OUT, value=0)
    out = "%s/%s" % (OUT, arm)
    _mkdirs(out)
    thumb = bytearray(THUMB_W * THUMB_H * 2)
    for mode in MODES:
        name = "%dx%d" % mode
        cam = _open(arm, mode)
        try:
            record = {
                "arm": arm,
                "mode": name,
                "sensor": cam.sensor(),
                "size": cam.size(),
                "columns": ["ms", "luma", "ae_brightness", "ae_target"] + ["0x%04X" % r for r in AE_REGS],
            }
            record["cold"] = _log(cam, thumb, SETTLE_MS)
            led(1)
            record["led_on"] = _log(cam, thumb, SETTLE_MS)
            led(0)
            record["led_off"] = _log(cam, thumb, SETTLE_MS)
            record["color_temp"] = cam.color_temp()
            record["wb_gains"] = cam.wb_gains()
            record["tuning"] = cam.tuning()
            record["controls"] = cam.controls()
            record["stats"] = cam.stats()
            record["regs"] = [[r, cam.reg(r)] for r in _mode_regs()]
            with open("%s/%s.jpg" % (out, name), "wb") as f:
                f.write(cam.capture_jpeg(95))
            cam.capture_scaled(thumb, THUMB_W, THUMB_H)
            with open("%s/%s.rgb565" % (out, name), "wb") as f:
                f.write(thumb)
            with open("%s/%s.json" % (out, name), "w") as f:
                json.dump(record, f)
            print(arm, name, "settled luma %.1f" % record["led_off"][-1][1])
        finally:
            led(0)
            cam.deinit()
