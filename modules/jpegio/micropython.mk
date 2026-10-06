# jpegio: CircuitPython-compatible JPEG decoder (TJpgDec R0.03, CP's config).
# Platform-neutral C, built on every port.
#
# The vendored src/tjpgd/tjpgd.c is the firmware's only TJpgDec. LVGL's own copy
# is off by config (LV_USE_TJPGD 0 in lvgl-bindings' lv_conf.h on MicroPython),
# so when lvgl-micropython is in the same build jpegio also compiles
# lvgl_decoder.c: an LVGL image decoder on this TJpgDec, registered through
# LVGL's public lv_image_decoder_create API. Without it nothing here changes
# and the build stays LVGL-less.

JPEGIO_MOD_DIR := $(USERMOD_DIR)
JPEGIO_DIR := $(JPEGIO_MOD_DIR)/src

CFLAGS_USERMOD += -I$(JPEGIO_DIR)/tjpgd

SRC_USERMOD_C += $(JPEGIO_DIR)/jpegio.c

# Library source: no qstrs, so it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(JPEGIO_DIR)/tjpgd/tjpgd.c

# lvgl-micropython is in this build if USER_C_MODULES names it -- either as a
# module directory itself (MicroPython 1.29 c_module()) or as a parent
# directory one level above it (py.mk's glob). Override with JPEGIO_LVGL=0/1
# on the make command line.
JPEGIO_LVMP_DIR := $(firstword $(filter %/lvgl-micropython,$(USER_C_MODULES:/=)) $(foreach d,$(USER_C_MODULES),$(wildcard $(d)/lvgl-micropython)))
JPEGIO_LVGL ?= $(if $(JPEGIO_LVMP_DIR),1,0)

ifeq ($(JPEGIO_LVGL),1)
# lvgl_decoder.c includes "lvgl/lvgl.h" and "lvgl/src/...", which resolve
# through the -I$(BINDINGS_DIR) lvgl-micropython adds, wherever its bindings
# are (a sibling checkout or its own fetched .deps/), so nothing here derives
# that path.
CFLAGS_USERMOD += -DJPEGIO_LVGL_DECODER=1
# No qstrs of its own (the Python-visible names live in jpegio.c).
SRC_USERMOD_LIB_C += $(JPEGIO_DIR)/lvgl_decoder.c
# lvgl.h with LV_USE_FLOAT trips -Werror=double-promotion / float-conversion
# on ports that append those after CFLAGS_USERMOD (unix, webassembly): the
# same per-object suppression lvgl-micropython puts on LVGL's own objects.
$(eval $(BUILD)/$(patsubst $(JPEGIO_MOD_DIR)/%,$(notdir $(JPEGIO_MOD_DIR))/%,$(JPEGIO_DIR)/lvgl_decoder.o): CFLAGS += -Wno-double-promotion -Wno-float-conversion)
endif
