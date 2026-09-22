# The micropython-vst3 sidecar interpreter on windows: no sockets, no SSL, no
# FFI (see the unix variant for why), the dev variant's ROM text compression,
# and SDL2 for displayif's usdl2. The engine's icon is not applied here; the
# installer does that.
PROG ?= mpvst-engine
MICROPY_ROM_TEXT_COMPRESSION = 1
override MICROPY_PY_SOCKET = 0
override MICROPY_PY_SSL = 0
override MICROPY_PY_FFI = 0
export SDL2_DEV ?= $(abspath $(VARIANT_DIR)/../../../../SDL2)
FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
