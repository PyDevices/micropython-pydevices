# synthstation, from omnipython-lib: the app that plays an audioinstruments
# instrument from a MIDI keyboard on controlpanel's panel. It needs LVGL,
# audiodsp, controlpanel and audioinstruments, so name them too (or use
# all,synthstation); the library brings midiports with it. Opt-in: all leaves
# it out (OPT_IN). Frozen at opt=3.
include("../library.py")
require("synthstation", library="omnipython-lib", opt=3)
