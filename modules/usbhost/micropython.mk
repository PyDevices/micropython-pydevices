# MicroPython Make-based build glue for usbhost (unix, windows).
# For CMake-based ports (esp32, rp2, …), see micropython.cmake in this dir.
#
# Named by c_module() in this repo's manifest.py (MicroPython 1.29+): add
#   include("<path to usbhost>/manifest.py")
# to your manifest, or point USER_C_MODULES at this directory.

USBHOST_MOD_DIR := $(USERMOD_DIR)
USBHOST_SRC_DIR := $(USBHOST_MOD_DIR)/src

CFLAGS_USERMOD += -I$(USBHOST_SRC_DIR)

SRC_USERMOD_C += \
    $(USBHOST_SRC_DIR)/mod_usbhost.c \
    $(USBHOST_SRC_DIR)/shared/usbhost_ringbuf.c

# --- which usbhost this firmware was built from ----------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
USBHOST_REVISION := $(shell git -C $(USBHOST_MOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DUSBHOST_REVISION='"$(USBHOST_REVISION)"'
