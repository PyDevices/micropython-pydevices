# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# The P4X EYE on the sensors-as-data spike: the modules you named, plus the
# OV2710 as a description generated from Espressif's driver, frozen in place
# of the C driver (which sdkconfig beside this file leaves out).
include("../../../../modules/manifest.py")
module("ov2710.py", base_path="../../../../modules/cameraif/sensors")
