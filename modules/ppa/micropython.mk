# ppa on the make ports: the software path only (the PPA is the ESP32-P4's,
# built by micropython.cmake). ppa_hw.c is the client code and, here, stubs.
PPA_DIR := $(USERMOD_DIR)/src

CFLAGS_USERMOD += -I$(PPA_DIR)

SRC_USERMOD_C += $(PPA_DIR)/mod_ppa.c

# No qstrs: they stay out of the QSTR scan.
SRC_USERMOD_LIB_C += $(PPA_DIR)/ppa_sw.c
SRC_USERMOD_LIB_C += $(PPA_DIR)/ppa_hw.c
