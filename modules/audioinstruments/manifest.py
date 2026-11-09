# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# audioinstruments, from omnipython-lib: the rack and its instruments, which
# the library builds from the PyDevices/audioinstruments repository at the tag
# its lib-lock.json pins. They import ulab and audiodsp, so name those too (or
# use all). Frozen at opt=3.
#
# AUDIOINSTRUMENTS in the environment, a comma list of instruments, freezes
# only those and what they import (selection.py, beside this file, works it
# out), with an `audioinstruments.ALL` that lists just them. Unset, the whole
# package.
import os
import sys

include("../library.py")
_picked = [n.strip() for n in os.environ.get("AUDIOINSTRUMENTS", "").split(",") if n.strip()]
if not _picked:
    require("audioinstruments", library="omnipython-lib", opt=3)
else:
    # A manifest runs with its own folder as the working directory. Loaded by
    # path under its own name: the other package's selection.py has the same
    # file name, and a plain import would find whichever came first.
    import importlib.util

    _spec = importlib.util.spec_from_file_location("audioinstruments_selection", os.path.join(os.getcwd(), "selection.py"))
    selection = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(selection)

    _library = os.environ["PYDEVICES_LIBRARY"]
    _out = os.path.join(os.path.dirname(_library), "audioinstruments-selected")
    _files = selection.write(os.path.join(_library, "audioinstruments", "audioinstruments"), _picked, _out)
    # On stderr: the build reads stdout for C module paths.
    print("audioinstruments: freezing %s (%d files)" % (", ".join(_picked), len(_files)), file=sys.stderr)
    package("audioinstruments", base_path=_out, opt=3)
