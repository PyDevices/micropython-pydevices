# pngio: PNG encode and decode. Platform-neutral C on every port.
PNGIO_DIR := $(USERMOD_DIR)/src

CFLAGS_USERMOD += -I$(PNGIO_DIR)

SRC_USERMOD_C += $(PNGIO_DIR)/mod_pngio.c

# The engine has no qstrs, so it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(PNGIO_DIR)/pngio_core.c
