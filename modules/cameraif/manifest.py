# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# MicroPython manifest for cameraif. The module is native; its Python half is
# the descriptions of the sensors whose C driver the ESP32-P4 images no longer
# carry (see sensors/ and cameraif_sensors.py): frozen here, read in place from
# flash, and tried by Camera() when no C driver answers.
import os

# MicroPython 1.29: the manifest names its own C module (workspace retool, piece 1).
c_module(".")  # this directory holds the micropython.cmake / micropython.mk for the C half

# Only the P4 has a MIPI camera for cameraif to open.
if os.environ.get("PYDEVICES_TARGET", "").startswith("esp32/ESP32_GENERIC_P4"):
    freeze("sensors", ("ov2710.py", "ov5647.py"))
    freeze("frozen", "cameraif_sensors.py")
