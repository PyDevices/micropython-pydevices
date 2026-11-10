# pngio: PNG encode and decode. Platform-neutral C on every port.
PNGIO_DIR := $(USERMOD_DIR)/src

CFLAGS_USERMOD += -I$(PNGIO_DIR)

# Which pngio this firmware was built from: pngio.__revision__.
PNGIO_REVISION := $(shell git -C $(USERMOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DPNGIO_REVISION='"$(PNGIO_REVISION)"'

SRC_USERMOD_C += $(PNGIO_DIR)/mod_pngio.c

# The engine has no qstrs, so it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(PNGIO_DIR)/pngio_core.c
