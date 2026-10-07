# The manifest a build_mp.py build freezes: upstream's own content for the
# port, then the modules you named. This is the user manifest MicroPython's
# manifest reference describes -- the port's base, then your additions -- and
# c_module() only takes effect through FROZEN_MANIFEST, so every module comes
# in here.
#
# The module list is a comma-separated string: options.modules if the
# including manifest passes one, else $PYDEVICES_MODULES, which build_mp.py
# sets from --modules. A bare name is modules/<name>; anything with a path
# separator is a directory of its own (build_mp.py makes it absolute). "all"
# is modules/all like any other name.
#
# Called by hand:
#   PYDEVICES_MODULES=audiodsp,displayif make -C ports/unix \
#       FROZEN_MANIFEST=<this repo>/modules/manifest.py
#
# options.base=False leaves the port's content out, for a manifest that brings
# its own (the webassembly variant's, whose Fetch-backed requests must be
# frozen before anything else resolves one).
import os

options.defaults(base=True, modules=None)

if options.base:
    # Board ports (esp32, rp2) carry a port-wide boards/manifest.py; variant
    # ports (unix, windows, webassembly) have no boards/ at all, so that
    # include fails on the path itself. Only THAT failure means "not a board
    # port"; any other error inside the port's manifest is real and is raised.
    # The port-wide file rather than $(BOARD_DIR)/manifest.py, because a board
    # directory of ours (boards/, until it retires) uses its own manifest.py
    # to carry a default, and including it back would include every module
    # twice.
    try:
        include("$(PORT_DIR)/boards/manifest.py")
    except Exception as _e:
        if "/boards/manifest.py" not in str(_e).replace("\\", "/"):
            raise
        # Upstream's default variant for the port, which includes the port's
        # own manifest and adds what the default build carries (asyncio on
        # unix and windows); the port-level manifest alone when a port has no
        # standard variant manifest.
        _included = False
        for _default in ("standard", "dev"):
            try:
                include("$(PORT_DIR)/variants/" + _default + "/manifest.py")
                _included = True
                break
            except Exception as _e2:
                if "/variants/" + _default + "/manifest.py" not in str(_e2).replace("\\", "/"):
                    raise
        if not _included:
            include("$(PORT_DIR)/variants/manifest.py")
    require("mip-cmdline")

_here = os.getcwd()  # include() runs a manifest from its own directory
_names = options.modules
if _names is None:
    _names = os.environ.get("PYDEVICES_MODULES", "")

for _name in (n.strip() for n in _names.split(",")):
    if not _name:
        continue
    if "/" in _name or os.sep in _name:
        _dir = _name
    else:
        _dir = os.path.join(_here, _name)
    # One spelling per module: upstream drops a duplicate C module only when
    # its path string matches, and a module reached through a symlink and
    # again through its real path would otherwise be compiled twice.
    _dir = os.path.realpath(_dir)
    if os.path.isfile(os.path.join(_dir, "manifest.py")):
        include(_dir)
        continue
    # A C module that carries no manifest of its own (ulab keeps its glue in
    # code/): its C half alone.
    for _c in (_dir, os.path.join(_dir, "code")):
        if os.path.isfile(os.path.join(_c, "micropython.mk")) or os.path.isfile(
            os.path.join(_c, "micropython.cmake")
        ):
            c_module(_c)
            break
    else:
        raise ValueError(
            "module '{}': {} has no manifest.py and no micropython.mk or micropython.cmake".format(
                _name, _dir
            )
        )
