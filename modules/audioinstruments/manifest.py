# audioinstruments, from omnipython-lib: the rack and its instruments, which
# the library builds from the PyDevices/audioinstruments repository at the tag
# its lib-lock.json pins. They import ulab and audiodsp, so name those too (or
# use all). Frozen at opt=3.
include("../library.py")
require("audioinstruments", library="omnipython-lib", opt=3)
