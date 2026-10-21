# Registers omnipython-lib, our library of Python-only packages, so a manifest
# can require("pydevices", library="omnipython-lib"). build_mp.py assembles the
# library tree (tools/libbuild.py build --tree-only in the pinned repository)
# and passes its directory in $PYDEVICES_LIBRARY.
#
# Include it from any manifest that requires from the library; it runs once
# however many include it. By hand, build the tree yourself and set
# PYDEVICES_LIBRARY to its library/ folder.
import os

_tree = os.environ.get("PYDEVICES_LIBRARY")
if not _tree:
    raise ValueError(
        "omnipython-lib: PYDEVICES_LIBRARY is not set. build_mp.py sets it; running "
        "this manifest by hand, point it at <out>/library from "
        "`python tools/libbuild.py build --tree-only --out <out>` in omnipython-lib"
    )
add_library("omnipython-lib", _tree)
