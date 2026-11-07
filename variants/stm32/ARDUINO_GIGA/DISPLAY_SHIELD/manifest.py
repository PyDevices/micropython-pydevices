# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# The DISPLAY_SHIELD variant's frozen content: the port's and the modules you
# named (modules/manifest.py), plus the part of the GIGA's own manifest this
# variant uses: the networking bundle (webrepl, ntptime, mip, requests) and
# logging. It leaves out aioble, which can't run with Bluetooth off, and the
# board's senml and msgpackrpc, to keep flash for LVGL.
include("../../../../modules/manifest.py", board_manifest=False)

require("bundle-networking")
require("logging")
