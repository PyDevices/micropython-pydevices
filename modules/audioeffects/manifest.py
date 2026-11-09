# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# audioeffects, from omnipython-lib: the effect classes, which the library
# builds from the PyDevices/audioeffects repository at the tag its
# lib-lock.json pins. They import ulab and audiodsp, so name those too (or
# use all). Frozen at opt=3.
#
# AUDIOEFFECTS in the environment, a comma list of effects (chorus,reverb),
# freezes only those and what they import (selection.py, beside this file,
# works it out), with an `audioeffects.ALL` that lists just them. Unset, the
# whole package.
import os
import sys

include("../library.py")
_picked = [n.strip() for n in os.environ.get("AUDIOEFFECTS", "").split(",") if n.strip()]
if not _picked:
    require("audioeffects", library="omnipython-lib", opt=3)
else:
    # A manifest runs with its own folder as the working directory. Loaded by
    # path under its own name: the other package's selection.py has the same
    # file name, and a plain import would find whichever came first.
    import importlib.util

    _spec = importlib.util.spec_from_file_location("audioeffects_selection", os.path.join(os.getcwd(), "selection.py"))
    selection = importlib.util.module_from_spec(_spec)
    _spec.loader.exec_module(selection)

    _library = os.environ["PYDEVICES_LIBRARY"]
    _out = os.path.join(os.path.dirname(_library), "audioeffects-selected")
    _files = selection.write(os.path.join(_library, "audioeffects", "audioeffects"), _picked, _out)
    # On stderr: the build reads stdout for C module paths.
    print("audioeffects: freezing %s (%d files)" % (", ".join(_picked), len(_files)), file=sys.stderr)
    package("audioeffects", base_path=_out, opt=3)
