# MicroPython Make-based build glue for usbhost (unix, windows), and
# CircuitPython's: every CircuitPython port builds with make.
# For CMake-based ports (esp32, rp2, …), see micropython.cmake in this dir.
#
# Named by c_module() in this repo's manifest.py (MicroPython 1.29+): add
#   include("<path to usbhost>/manifest.py")
# to your manifest, or point USER_C_MODULES at this directory.
#
# Most builds here get `_usbhost` with no host engine: the module imports, and
# its capabilities are an empty set. CircuitPython's ESP32-P4 builds get the
# whole host, on the chip's high-speed USB controller, which CircuitPython
# leaves alone on the boards that run it as a host:
#
#   make -C ports/espressif BOARD=<an ESP32-P4 board> USER_C_MODULES=/path/to/usbhost \
#       USER_ESP_IDF_COMPONENT_DIRS=/path/to/usb
#
# The engine runs on ESP-IDF's USB host library. From ESP-IDF 6, which
# CircuitPython's espressif port builds with, that's Espressif's `usb`
# component from its registry rather than part of ESP-IDF. CircuitPython builds
# with the component manager off, so micropython-pydevices' build_mp.py fetches
# it at the version deps.lock pins and passes it; one of its CircuitPython
# patches has the port build and link a component a module names. The host's
# settings (hubs, the 4 KB control-transfer buffer in ./sdkconfig) go in the
# board's own sdkconfig there.

USBHOST_MOD_DIR := $(USERMOD_DIR)
USBHOST_SRC_DIR := $(USBHOST_MOD_DIR)/src

CFLAGS_USERMOD += -I$(USBHOST_SRC_DIR)

SRC_USERMOD_C += \
    $(USBHOST_SRC_DIR)/mod_usbhost.c \
    $(USBHOST_SRC_DIR)/shared/usbhost_ringbuf.c

USBHOST_ENGINE :=
ifneq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifeq ($(IDF_TARGET),esp32p4)
USBHOST_ENGINE := 1
endif
endif

ifeq ($(USBHOST_ENGINE),1)
USBHOST_USB := $(filter %/usb,$(USER_ESP_IDF_COMPONENT_DIRS:/=))
ifeq ($(USBHOST_USB),)
$(error usbhost: USER_ESP_IDF_COMPONENT_DIRS names no usb. ESP-IDF 6 doesn't ship the USB host library; build with micropython-pydevices' build_mp.py, or pass the directory of Espressif's usb component)
endif

SRC_USERMOD_C += \
    $(USBHOST_SRC_DIR)/usbhost_host.c \
    $(USBHOST_SRC_DIR)/usbhost_host_xfer.c \
    $(USBHOST_SRC_DIR)/usbhost_host_stream.c \
    $(USBHOST_SRC_DIR)/shared/usbhost_pcm_sink.c

# The USB host library builds a library, and the port links it.
USER_ESP_IDF_COMPONENTS += usb
# CircuitPython compiles modules outside ESP-IDF's CMake, so the component's
# public headers are named here.
CFLAGS_USERMOD += -isystem $(USBHOST_USB)/include

# CircuitPython's audiodsp builds without its audio pump, so a stream can't
# feed an audiopump.Ring there, even when the pump's header is on the path.
CFLAGS_USERMOD += -DUSBHOST_NO_AUDIOPUMP=1

# A phone's USB tethering becomes an ESP-IDF network interface, which needs
# lwIP. CircuitPython's ESP32-P4 port has no network yet (it doesn't drive the
# boards' ESP32-C6), so there it's left out and Tether() says so.
ifeq ($(CIRCUITPY_WIFI),0)
CFLAGS_USERMOD += -DUSBHOST_NO_NETWORK=1
else
SRC_USERMOD_C += $(USBHOST_SRC_DIR)/usbhost_host_net.c
endif
else
# Built without the engine, even on a chip with a USB OTG controller (a
# CircuitPython ESP32-S3, whose one controller is its drive and REPL).
CFLAGS_USERMOD += -DUSBHOST_NO_HOST_ENGINE=1
endif

# --- which usbhost this firmware was built from ----------------------
# Computed at build time from this repo's own git, never stored; "unknown" when
# there is no git (a tarball). Read on a target as <module>.__revision__.
USBHOST_REVISION := $(shell git -C $(USBHOST_MOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DUSBHOST_REVISION='"$(USBHOST_REVISION)"'
