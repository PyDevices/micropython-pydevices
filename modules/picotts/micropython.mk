# picotts: the SVOX Pico text-to-speech engine (third_party/svox, Apache-2.0)
# as the _picotts C module, under the frozen picotts.py.
#
# svox is built as-is apart from Adafruit's platform guards. PICO_PLATFORM
# selects svox's file-less platform (the one picopltf.h calls PICO_CIRCUITPY):
# the voice is handed to the engine in memory, so it never opens a file. The
# engine is third-party C, so its warnings are silenced for its objects only.
#
# MicroPython only: on CircuitPython use Adafruit's adafruit_picotts library.

ifeq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)

PICOTTS_MOD_DIR := $(USERMOD_DIR)
PICOTTS_SVOX := $(PICOTTS_MOD_DIR)/third_party/svox

CFLAGS_USERMOD += -I$(PICOTTS_SVOX) -DPICO_PLATFORM=PICO_CIRCUITPY

SRC_USERMOD_C += $(PICOTTS_MOD_DIR)/src/modpicotts.c

PICOTTS_SVOX_SRC := $(wildcard $(PICOTTS_SVOX)/*.c)
SRC_USERMOD_LIB_C += $(PICOTTS_SVOX_SRC)

$(foreach f,$(PICOTTS_SVOX_SRC),$(eval $(BUILD)/$(patsubst $(PICOTTS_MOD_DIR)/%,$(notdir $(PICOTTS_MOD_DIR))/%,$(f:.c=.o)): CFLAGS += -w))

endif
