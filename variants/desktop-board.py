# The desktop and browser images' board config, frozen, from omnipython-lib:
# board_config and board_peripherals from its `desktop` package, and the
# desktop-only helpers pydevices-desktop carries (the pure-Python SDL2 and
# Win32 bindings the display, audio and timer backends open a window and a
# sound device through, and frame_recorder). AutoDisplay picks SDL, Windows
# GDI or the browser canvas when the config is imported. A board_config.py
# beside your script still wins: the script's directory comes before .frozen
# on sys.path. Board images never include this file.
#
# The package manifests aren't required: `desktop` requires all of
# pydevices-desktop, whose mip.py, pngio.py and mdns.py would stand in front
# of the firmware's own mip and the C modules of those names.
import os

include("../modules/library.py")
_tree = os.environ["PYDEVICES_LIBRARY"]
for _name in ("board_config.py", "board_peripherals.py"):
    module(_name, base_path=os.path.join(_tree, "desktop"), opt=3)
for _name in ("usdl2.py", "uwin32.py", "frame_recorder.py"):
    module(_name, base_path=os.path.join(_tree, "pydevices-desktop"), opt=3)
