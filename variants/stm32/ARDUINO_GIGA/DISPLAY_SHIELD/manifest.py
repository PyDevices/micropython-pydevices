# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# The DISPLAY_SHIELD variant's Python: the port's and the modules you named
# (modules/manifest.py), plus the part of the GIGA's own manifest this variant
# uses: the networking bundle (webrepl, ntptime, mip, requests) and logging. It
# leaves out aioble, which can't run with Bluetooth off, and the board's senml
# and msgpackrpc.
import os

include("../../../../modules/manifest.py", board_manifest=False)

require("bundle-networking")
require("logging")

# The USB device classes a program adds from code with machine.USBDevice, the
# set modules/micropython-lib/manifest.py freezes elsewhere, with usb-device-msc
# for a drive (the port starts as the REPL alone). They go in the ROMFS as a
# set, because a usb package found in /rom hides any usb/device/ in /lib. There
# is no usb-device-net: this port has no NCM driver or network interface to
# hand it frames.
for _pkg in (
    "usb-device",
    "usb-device-cdc",
    "usb-device-hid",
    "usb-device-keyboard",
    "usb-device-midi",
    "usb-device-mouse",
):
    require(_pkg)
include("../../../../modules/library.py")
require("usb-device-msc", library="omnipython-lib")

# Built by build_mp.py, none of it is frozen: every Python module above goes
# to the board's ROMFS partition on its QSPI flash, and internal flash is left
# to LVGL and the C modules. This writes the list for build_mp.py, which
# compiles it for the board into romfs.img beside the firmware. ROMFS is
# mounted at boot and /rom is on sys.path, so the modules import as before.
_python_to_romfs = os.environ.get("PYDEVICES_PYTHON_TO_ROMFS")
_manifest = include.__self__
# The pass that only collects C modules lists no files: leave the list alone.
if _python_to_romfs and _manifest.files():
    import json

    with open(_python_to_romfs, "w") as _f:
        json.dump([[_o.full_path, _o.target_path, _o.opt] for _o in _manifest.files()], _f)
    del _manifest.files()[:]
