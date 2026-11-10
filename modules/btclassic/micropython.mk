# btclassic on the unix port: no radio, but the whole binding on a loopback
# backend (host/btclassic_host.c), so its tests run on a desktop. The host
# backend's `_btclassic_host` module plays the phone: it feeds the sink and
# the call, and takes what the board sends back. Every other Make port skips
# the module.
ifeq ($(notdir $(CURDIR)),unix)
BTCLASSIC_MOD_DIR := $(USERMOD_DIR)
SRC_USERMOD_C += $(BTCLASSIC_MOD_DIR)/src/mod_btclassic.c
SRC_USERMOD_C += $(BTCLASSIC_MOD_DIR)/esp-idf/btclassic/ring.c
SRC_USERMOD_C += $(BTCLASSIC_MOD_DIR)/host/btclassic_host.c
CFLAGS_USERMOD += -I$(BTCLASSIC_MOD_DIR)/esp-idf/btclassic/include -I$(BTCLASSIC_MOD_DIR)/esp-idf/btclassic -DBTC_HOST=1

# Which btclassic this firmware was built from: btclassic.__revision__.
BTCLASSIC_REVISION := $(shell git -C $(BTCLASSIC_MOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DBTCLASSIC_REVISION='"$(BTCLASSIC_REVISION)"'

# With audiodsp in the build, AudioSink and HandsFree are also sources the
# audio pump pulls, as on a board.
BTCLASSIC_AUDIODSP_DIR := $(patsubst %/src/audiocore/__init__.h,%,$(firstword $(foreach _d,$(USER_C_MODULES),\
    $(wildcard $(_d)/src/audiocore/__init__.h $(_d)/*/src/audiocore/__init__.h))))
ifneq ($(BTCLASSIC_AUDIODSP_DIR),)
CFLAGS_USERMOD += -I$(BTCLASSIC_AUDIODSP_DIR)/src -DBTCLASSIC_AUDIOSAMPLE=1
endif
endif
