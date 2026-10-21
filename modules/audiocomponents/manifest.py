# audiocomponents' Python, from omnipython-lib: its instruments and its
# effects, which the library builds from audiocomponents at the tag its
# lib-lock.json pins. They import ulab and audiodsp, so name those too (or
# use all). Frozen at opt=3, as before.
include("../library.py")
require("audioinstruments", library="omnipython-lib", opt=3)
require("audioeffects", library="omnipython-lib", opt=3)
