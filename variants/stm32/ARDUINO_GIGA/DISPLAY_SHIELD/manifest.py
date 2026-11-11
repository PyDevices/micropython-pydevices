# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# The DISPLAY_SHIELD variant's frozen content: the port's and the modules you
# named (modules/manifest.py), plus the part of the GIGA's own manifest this
# variant uses: the networking bundle (webrepl, ntptime, mip, requests) and
# logging. It leaves out aioble, which can't run with Bluetooth off, and the
# board's senml and msgpackrpc, to keep flash for LVGL.
import os

include("../../../../modules/manifest.py", board_manifest=False)

require("bundle-networking")
require("logging")

# build_mp.py --python-to-romfs: every Python module above goes to the board's
# ROMFS partition on its QSPI flash instead of internal flash, which LVGL and
# the C modules need. This writes the list for build_mp.py, which compiles it
# for the board, and freezes nothing. ROMFS is mounted at boot and /rom is on
# sys.path, so the modules import as before.
_python_to_romfs = os.environ.get("PYDEVICES_PYTHON_TO_ROMFS")
_manifest = include.__self__
# The pass that only collects C modules lists no files: leave the list alone.
if _python_to_romfs and _manifest.files():
    import json

    with open(_python_to_romfs, "w") as _f:
        json.dump([[_o.full_path, _o.target_path, _o.opt] for _o in _manifest.files()], _f)
    del _manifest.files()[:]
