# The mpvst sidecar interpreter on windows: no sockets, no SSL, no
# FFI (see the unix variant for why; mpconfigvariant.h is where the C side is
# switched off), the dev variant's ROM text compression,
# SDL2 for displayif's usdl2, and the engine's own icon.
PROG ?= mpvst-engine
MICROPY_ROM_TEXT_COMPRESSION = 1
override MICROPY_PY_SOCKET = 0
override MICROPY_PY_SSL = 0
override MICROPY_PY_FFI = 0
export SDL2_DEV ?= $(abspath $(VARIANT_DIR)/../../../../SDL2)
FROZEN_MANIFEST ?= $(VARIANT_DIR)/manifest.py

# The executable's icon. The port compiles its own micropython.rc (MicroPython's
# logo) into $(BUILD)/micropython.res through a pattern rule; an explicit rule
# for that target wins, so passing ENGINE_ICON=<path to a .ico> on the make
# line (mpvst's scripts/build-micropython-engine.sh does) swaps the icon
# without touching the checkout. Without ENGINE_ICON the port's rule applies.
ifdef ENGINE_ICON
$(BUILD)/micropython.res: $(ENGINE_ICON)
	$(ECHO) "WINDRES $< (engine icon)"
	$(Q)printf 'app     ICON    "%s"\n' "$(abspath $(ENGINE_ICON))" > $(BUILD)/engine.rc
	$(Q)$(WINDRES) $(BUILD)/engine.rc -O coff -o $@
endif
