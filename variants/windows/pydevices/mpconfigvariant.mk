# The PyDevices windows build: upstream's dev variant (ROM text compression)
# plus what displayif's usdl2 backend needs to link -- SDL2's MinGW
# development tree, which the work directory carries as SDL2/ beside the
# MicroPython clone. Cross-compiled with CROSS_COMPILE=x86_64-w64-mingw32-.
MICROPY_ROM_TEXT_COMPRESSION = 1
# The overlay's Windows networking patch (0001) gives the dev variant SSL over
# mbedtls; the same two lines, since this variant stands in for dev.
MICROPY_PY_SSL = 1
MICROPY_SSL_MBEDTLS = 1
export SDL2_DEV ?= $(abspath $(VARIANT_DIR)/../../../../SDL2)
FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py
