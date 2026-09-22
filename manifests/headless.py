# Upstream's own frozen content for this port, whichever kind of port it is.
# Board ports (esp32, rp2) carry a port-wide boards/manifest.py; variant ports
# (unix, windows, webassembly) have no boards/ at all, so that include fails on
# the path itself. Only THAT failure means "not a board port"; any other error
# inside the port's manifest is real and is raised. The port-wide file rather
# than $(BOARD_DIR)/manifest.py, because a board directory of ours uses its
# own manifest.py to carry a default preset, and a preset including it back
# would include every module twice.
try:
    include("$(PORT_DIR)/boards/manifest.py")
    _board_port = True
except Exception as _e:
    _msg = str(_e)
    if "/boards/manifest.py" in _msg:
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
                if "/variants/" + _default + "/manifest.py" not in str(_e2):
                    raise
        if not _included:
            include("$(PORT_DIR)/variants/manifest.py")
        _board_port = False
    else:
        raise
require("mip-cmdline")

