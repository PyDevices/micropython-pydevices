# svgio: SVG to RGB565 and alpha on NanoSVG. Platform-neutral C on every port.
#
# When LVGL is in the same build, svgio also compiles lvgl_decoder.c, an LVGL
# image decoder on the same NanoSVG, registered through LVGL's public
# lv_image_decoder_create API, as jpegio does.

SVGIO_MOD_DIR := $(USERMOD_DIR)
SVGIO_DIR := $(SVGIO_MOD_DIR)/src

CFLAGS_USERMOD += -I$(SVGIO_DIR) -I$(SVGIO_MOD_DIR)/third_party/nanosvg

# Which svgio this firmware was built from: svgio.__revision__.
SVGIO_REVISION := $(shell git -C $(USERMOD_DIR) describe --always --dirty --abbrev=7 2>/dev/null || echo unknown)
CFLAGS_USERMOD += -DSVGIO_REVISION='"$(SVGIO_REVISION)"'

SRC_USERMOD_C += $(SVGIO_DIR)/mod_svgio.c

# The engine has no qstrs, so it stays out of the QSTR scan.
SRC_USERMOD_LIB_C += $(SVGIO_DIR)/svgio_nanosvg.c

# LVGL is in this build if USER_C_MODULES names omnipython-lvgl's
# ports/micropython, whose repository root, two levels up, holds the generated
# binding. Override with SVGIO_LVGL=0/1 on the make command line.
SVGIO_LVMP_DIR := $(firstword $(foreach d,$(USER_C_MODULES:/=),$(if $(wildcard $(d)/../../generated/lvgl_micropython.c),$(d))))
SVGIO_LVGL ?= $(if $(SVGIO_LVMP_DIR),1,0)

ifeq ($(SVGIO_LVGL),1)
CFLAGS_USERMOD += -DSVGIO_LVGL_DECODER=1
SRC_USERMOD_LIB_C += $(SVGIO_DIR)/lvgl_decoder.c
# lvgl.h with LV_USE_FLOAT trips -Werror=double-promotion / float-conversion
# on ports that append those after CFLAGS_USERMOD (unix, webassembly).
$(eval $(BUILD)/$(patsubst $(SVGIO_MOD_DIR)/%,$(notdir $(SVGIO_MOD_DIR))/%,$(SVGIO_DIR)/lvgl_decoder.o): CFLAGS += -Wno-double-promotion -Wno-float-conversion)
endif
