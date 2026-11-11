# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The sensor descriptions frozen into this image.

Camera() imports each of these, in this order, when no sensor is passed as
`sensor=`: it opens the description's I2C address and checks its chip ID, and
drops the module again if no camera answers. A sensor not listed here is a file
to copy to the board and pass as `sensor=`.
"""

NAMES = (
    "ov5647", "ov2710",
    "gc2053", "gc2607", "gc4053", "jxh63p", "mira220", "mit245", "os02n10",
    "os04c10", "ov9281", "sc030iot", "sc035hgs", "sc132gs", "sc1346", "sc202cs",
    "sc2331", "sc2336", "sc2337p", "sti2250",
)
