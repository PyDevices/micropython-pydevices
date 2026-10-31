# audioeffects, from omnipython-lib: the effect classes, which the library
# builds from the PyDevices/audioeffects repository at the tag its
# lib-lock.json pins. They import ulab and audiodsp, so name those too (or
# use all). Frozen at opt=3.
include("../library.py")
require("audioeffects", library="omnipython-lib", opt=3)
