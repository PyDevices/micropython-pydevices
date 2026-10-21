# castif on Makefile-built firmware, which here means CircuitPython.
#
# MicroPython's Makefile ports get nothing from this file: castif is the
# ESP32-P4's, and MicroPython's esp32 port builds it through micropython.cmake.
# CircuitPython builds its espressif port with make, so on an ESP32-P4 castif
# is built in here. It talks to lwIP directly, under CircuitPython's own
# socketpool, and encodes and muxes through h264enc and tsmux: name all three
# (`--modules castif,h264enc,tsmux,ppa`), or Cast() and Hls() say which is
# missing.
#
# CircuitPython's ESP32-P4 port has no network yet (it doesn't drive the
# boards' ESP32-C6 radio), so lwIP isn't in its firmware. castif is then built
# with CASTIF_NO_NETWORK: castif_nonet.c stands in for the few lwIP calls it
# makes, and start() says there is no network rather than sending nothing.
#
# On any other CircuitPython target, unix included, castif isn't built and
# there is no module to import.

ifneq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifeq ($(IDF_TARGET),esp32p4)

CASTIF_MOD_DIR := $(USERMOD_DIR)

SRC_USERMOD_C += $(CASTIF_MOD_DIR)/src/mod_castif.c $(CASTIF_MOD_DIR)/src/castif_hls.c
CFLAGS_USERMOD += -I$(CASTIF_MOD_DIR)/src

ifeq ($(CIRCUITPY_WIFI),0)
CFLAGS_USERMOD += -DCASTIF_NO_NETWORK=1
# No qstrs: it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(CASTIF_MOD_DIR)/src/castif_nonet.c
endif

endif
endif
