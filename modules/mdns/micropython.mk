# mdns: CircuitPython's mdns API. On the make ports it builds for unix (our
# own responder and querier) and windows (the system's DNS-SD API) only: the
# browser can't send multicast, and CircuitPython has its own mdns module.
MDNS_DIR := $(USERMOD_DIR)/src
MDNS_PORT_DIR := $(abspath $(CURDIR))

ifeq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifneq ($(findstring /ports/unix,$(MDNS_PORT_DIR))$(findstring /ports/windows,$(MDNS_PORT_DIR)),)

CFLAGS_USERMOD += -I$(MDNS_DIR)
SRC_USERMOD_C += $(MDNS_DIR)/mod_mdns.c
# No qstrs: they stay out of the QSTR scan.
SRC_USERMOD_LIB_C += $(MDNS_DIR)/mdns_posix.c
SRC_USERMOD_LIB_C += $(MDNS_DIR)/mdns_windows.c

ifneq ($(findstring /ports/windows,$(MDNS_PORT_DIR)),)
LIBS_USERMOD += -ldnsapi -lws2_32
else
LIBS_USERMOD += -lpthread
endif

endif
endif
