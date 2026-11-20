# offheap on the make ports: unix, windows and webassembly, where the bytes
# come from C's malloc. The other make ports (stm32, mimxrt, samd, nrf) give
# their RAM to the GC heap and have no allocator beside it, so nothing is
# built there and `import offheap` raises ImportError. MicroPython only:
# CircuitPython (its py/ has circuitpy_mpconfig.h) gets nothing either.
OFFHEAP_DIR := $(USERMOD_DIR)/src
OFFHEAP_PORT_DIR := $(abspath $(CURDIR))

ifeq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifneq ($(findstring /ports/unix,$(OFFHEAP_PORT_DIR))$(findstring /ports/windows,$(OFFHEAP_PORT_DIR))$(findstring /ports/webassembly,$(OFFHEAP_PORT_DIR)),)

# Which offheap this firmware was built from: offheap.__revision__.
OFFHEAP_REVISION := $(shell git -C $(USERMOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DOFFHEAP_REVISION='"$(OFFHEAP_REVISION)"'
SRC_USERMOD_C += $(OFFHEAP_DIR)/mod_offheap.c

endif
endif
