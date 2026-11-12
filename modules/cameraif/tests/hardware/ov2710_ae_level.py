# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Set every auto-exposure level on an OV2710 and read its six registers back.

Run it on a board whose camera is an OV2710 (the ESP32-P4X-EYE), with nothing
else holding the camera. Each level must leave the registers Espressif's driver
writes for it, and a level outside 2..235 must be clamped, not written raw.

    mpftp run --follow -d COM119 ov2710_ae_level.py
"""

import board_config

cam = board_config.camera
registers = (0x3A0F, 0x3A10, 0x3A1B, 0x3A1E, 0x3A11, 0x3A1F)
bad = 0
for level in range(2, 236):
    cam.ae_level = level
    high = level * 27 // 25
    low = level * 23 // 25
    want = (high & 255, low & 255, (high + 1) & 255, (low - 1) & 255, min(high << 1, 255), low >> 1)
    got = tuple(cam.reg(r) for r in registers)
    if got != want or cam.ae_level != level:
        bad += 1
        print("level", level, "wanted", want, "read", got, cam.ae_level)
cam.ae_level = 1
cam.ae_level = 300
assert cam.ae_level == 235
print("234 levels,", bad, "wrong")
assert bad == 0
