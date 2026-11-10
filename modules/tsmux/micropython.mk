# tsmux: portable C, built on every port. tsmux_core.c is the muxer with no
# MicroPython in it (castif links the same file on the P4); mod_tsmux.c is the
# Python binding.
TSMUX_DIR := $(USERMOD_DIR)/src

CFLAGS_USERMOD += -I$(TSMUX_DIR)

# Which tsmux this firmware was built from: tsmux.__revision__.
TSMUX_REVISION := $(shell git -C $(USERMOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DTSMUX_REVISION='"$(TSMUX_REVISION)"'

SRC_USERMOD_C += $(TSMUX_DIR)/mod_tsmux.c

# No qstrs: it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(TSMUX_DIR)/tsmux_core.c
