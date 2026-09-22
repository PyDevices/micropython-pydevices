# The micropython-vst3 sidecar interpreter on unix: no sockets, no SSL, no
# FFI -- compositions are code and some of it runs at plugin-scan time, so the
# engine cannot reach the network or arbitrary native libraries. `override`
# because the port's mpconfigport.mk assigns these with a plain `=` after this
# file is read.
PROG ?= mpvst-engine
override MICROPY_PY_SOCKET = 0
override MICROPY_PY_SSL = 0
override MICROPY_PY_FFI = 0
FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
