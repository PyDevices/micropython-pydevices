# The direct browser runtime. This variant brings its own port content rather
# than modules/manifest.py's:
# its Fetch-backed requests module must be frozen before anything resolves
# the socket-backed one, because frozen module lookup keeps the first match.
include("$(PORT_DIR)/variants/manifest.py")
freeze(".", "requests.py", opt=3)
# The port's patched main.c calls pydevices_bridge_deinit() (overlay 0006),
# which the wasm bridge usermod beside this variant provides.
c_module("../wasmbridge")
# mip without resolving its socket-based requests dependency; the sources
# import the Fetch facade above at runtime.
require("argparse")
freeze("$(MPY_LIB_DIR)/micropython/mip", ("mip/__init__.py",), opt=3)
freeze("$(MPY_LIB_DIR)/micropython/mip-cmdline", ("mip/__main__.py",), opt=3)
# Then the modules you named, without modules/manifest.py's own port content:
# that would pull in mip-cmdline and its socket-backed requests before the
# Fetch-backed one above, and frozen lookup keeps the first match.
include("../../../modules", base=False)
