# stm32 port — portable spibus/i2cbus (+ rgbmatrix bitbang); MIPI DSI on the
# H747/H757 (DSI host + LTDC); no DotClock/i80 yet.

CFLAGS_USERMOD += -I$(DISPLAYIF_MOD_DIR)/src/ports/stm32

SRC_USERMOD_C += \
    $(DISPLAYIF_MOD_DIR)/src/ports/common/notimpl/mod_dotclockframebuffer.c \
    $(DISPLAYIF_MOD_DIR)/src/ports/common/notimpl/mod_mipidsi.c \
    $(DISPLAYIF_MOD_DIR)/src/ports/common/notimpl/mod_i80bus.c \
    $(DISPLAYIF_MOD_DIR)/src/ports/common/notimpl/mod_qspibus.c

# MIPI DSI: the H747/H757 have a DSI host. The board's stm32h7xx_hal_conf.h
# must define HAL_DSI_MODULE_ENABLED and include the DSI and LTDC HAL headers,
# and its make settings must add hal_dsi.c and hal_ltdc.c to HAL_SRC_C
# (STM32LIB_HAL_BASE is not defined yet when user modules are included).
ifneq ($(filter STM32H747xx STM32H757xx,$(CMSIS_MCU)),)
SRC_USERMOD_C := $(filter-out $(DISPLAYIF_MOD_DIR)/src/ports/common/notimpl/mod_mipidsi.c,$(SRC_USERMOD_C))
SRC_USERMOD_C += $(DISPLAYIF_MOD_DIR)/src/ports/stm32/mod_mipidsi.c
endif
