# h264enc on Makefile-built firmware, which here means CircuitPython.
#
# MicroPython's Makefile ports (unix, rp2, ...) get nothing from this file:
# the encoder is the ESP32-P4's hardware, and MicroPython's esp32 port builds
# h264enc through micropython.cmake. CircuitPython builds every port with make,
# its espressif port included, so this is its route in:
#
#   make -C ports/espressif BOARD=<an ESP32-P4 board> USER_C_MODULES="/path/to/h264enc /path/to/ppa" \
#       USER_ESP_IDF_COMPONENT_DIRS=/path/to/esp_h264
#
# esp_h264 is Espressif's encoder component, from its component registry.
# CircuitPython builds with the component manager off, so micropython-pydevices'
# build_mp.py fetches it at the version deps.lock pins (the one
# components/h264enc_esp_h264 asks MicroPython's build for) and passes it; one
# of its CircuitPython patches has the port build the component and link it.
# The colour conversion is ppa's: name both modules, or Encoder() says ppa is
# missing.
#
# On any other CircuitPython target, unix included, h264enc isn't built and
# there is no module to import.

ifneq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifeq ($(IDF_TARGET),esp32p4)

H264ENC_MOD_DIR := $(USERMOD_DIR)
H264ENC_ESP_H264 := $(filter %/esp_h264,$(USER_ESP_IDF_COMPONENT_DIRS:/=))
ifeq ($(H264ENC_ESP_H264),)
$(error h264enc: USER_ESP_IDF_COMPONENT_DIRS names no esp_h264. ESP-IDF doesn't ship the H.264 encoder; build with micropython-pydevices' build_mp.py, or pass the esp_h264 component's directory)
endif

SRC_USERMOD_C += $(H264ENC_MOD_DIR)/src/mod_h264enc.c
CFLAGS_USERMOD += -I$(H264ENC_MOD_DIR)/src

# The component builds a library, and the port links it.
USER_ESP_IDF_COMPONENTS += esp_h264

# CircuitPython compiles modules outside ESP-IDF's CMake, so the component's
# public include directories are named here.
CFLAGS_USERMOD += \
	-isystem $(H264ENC_ESP_H264)/interface/include \
	-isystem $(H264ENC_ESP_H264)/port/include \
	-isystem $(H264ENC_ESP_H264)/hw/include \
	-isystem $(H264ENC_ESP_H264)/sw/include

endif
endif
