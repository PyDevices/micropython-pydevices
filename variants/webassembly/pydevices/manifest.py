# The direct browser runtime. This variant does not use a preset's prologue:
# its Fetch-backed requests module must be frozen before anything resolves
# the socket-backed one, because frozen module lookup keeps the first match.
include("$(PORT_DIR)/variants/manifest.py")
freeze(".", "requests.py", opt=3)
# The port's patched main.c calls pydevices_bridge_deinit() (overlay 0006),
# which the wasm bridge usermod beside these variants provides.
c_module("../../../usermods/wasmbridge")
# mip without resolving its socket-based requests dependency; the sources
# import the Fetch facade above at runtime.
require("argparse")
freeze("$(MPY_LIB_DIR)/micropython/mip", ("mip/__init__.py",), opt=3)
freeze("$(MPY_LIB_DIR)/micropython/mip-cmdline", ("mip/__main__.py",), opt=3)
# The kitchen sink's sibling scan, repeated here rather than included: that
# preset's prologue would pull in mip-cmdline and its socket-backed requests
# before the Fetch-backed one above, and frozen lookup keeps the first match.
# Its root is this repository's parent, three levels up from this variant.
# Every sibling repository that carries a manifest.py, found rather than
# listed. A sibling is included when it has a micropython.mk, or lacks an
# apply_cp_patches.sh (which marks a CircuitPython-only tree that would
# double-freeze shared helpers). Nothing here names a module; each module's
# own manifest names its C half. pydevices is installed with mip and never
# frozen -- a frozen copy silently shadows the published one -- and its
# manifest.py packages the tree for mip, so it is skipped.
import os

_root = os.path.abspath(os.path.join(os.getcwd(), "..", "..", "..", ".."))
for _name in sorted(os.listdir(_root)):
    if _name.startswith(".") or _name in ("micropython", "micropython-pydevices", "pydevices"):
        continue
    _dir = os.path.join(_root, _name)
    _path = os.path.join(_dir, "manifest.py")
    if not os.path.isfile(_path):
        continue
    _has_mp = os.path.isfile(os.path.join(_dir, "micropython.mk"))
    _has_cp_patches = os.path.isfile(os.path.join(_dir, "apply_cp_patches.sh"))
    if not (_has_mp or not _has_cp_patches):
        continue
    include(_path)
