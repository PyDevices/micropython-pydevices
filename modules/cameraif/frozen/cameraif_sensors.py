# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The sensor descriptions frozen into this image.

Camera() tries each one, in this order, when none of the sensor drivers built
into the firmware answers: it opens the description's I2C address and checks
its chip ID. A sensor not listed here is a file to copy to the board and pass
as `sensor=`.
"""

from ov2710 import SENSOR as _ov2710
from ov5647 import SENSOR as _ov5647

SENSORS = (_ov2710, _ov5647)
