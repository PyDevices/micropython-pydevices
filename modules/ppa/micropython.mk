# ppa on the make ports: the software path only (the PPA is the ESP32-P4's,
# built by micropython.cmake). ppa_hw.c is the client code and, here, stubs.
PPA_DIR := $(USERMOD_DIR)/src

CFLAGS_USERMOD += -I$(PPA_DIR)

SRC_USERMOD_C += $(PPA_DIR)/mod_ppa.c

# No qstrs: they stay out of the QSTR scan.
SRC_USERMOD_LIB_C += $(PPA_DIR)/ppa_sw.c
SRC_USERMOD_LIB_C += $(PPA_DIR)/ppa_hw.c

# CircuitPython builds its espressif port with make, so on an ESP32-P4 the PPA
# is built in here, as micropython.cmake does on MicroPython. CircuitPython's
# espressif port builds and links the components a module names
# (micropython-pydevices' CircuitPython patches).
ifneq ($(wildcard $(TOP)/py/circuitpy_mpconfig.h),)
ifeq ($(IDF_TARGET),esp32p4)
CFLAGS_USERMOD += -DPPA_HW=1 \
	-isystem $(TOP)/ports/espressif/esp-idf/components/esp_driver_ppa/include \
	-isystem $(TOP)/ports/espressif/esp-idf/components/esp_hal_ppa/include \
	-isystem $(TOP)/ports/espressif/esp-idf/components/esp_hal_ppa/esp32p4/include
USER_ESP_IDF_COMPONENTS += esp_driver_ppa esp_hal_ppa
endif
endif
