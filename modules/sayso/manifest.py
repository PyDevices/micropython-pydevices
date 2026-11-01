# sayso, from omnipython-lib: the Spotify controller. Desktop and web builds
# only (unix, windows, webassembly): a board build that names it, or all,
# freezes nothing. Frozen at opt=3.
import os

if os.environ.get("PYDEVICES_PLATFORM") != "board":
    include("../library.py")
    require("sayso", library="omnipython-lib", opt=3)
