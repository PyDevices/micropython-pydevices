# Arduino GIGA R1 WiFi with the GIGA Display Shield: upstream's ARDUINO_GIGA,
# plus what LVGL and the shield's MIPI DSI panel need, in the board's 1280 KB
# firmware slot. build_mp.py includes this after the board's own
# mpconfigboard.mk.

# The Wi-Fi chip's firmware (about 420 KB) moves out of internal flash into the
# megabyte of QSPI flash the board reserves for it at 0x90F00000, where the
# driver reads it in place. It is a second image, firmware1.bin, flashed once
# with dfu-util's alt setting 1; firmware0.bin is the rest, for internal flash.
LD_FILES += $(BOARD_DIR)/cyw43_qspi.ld
TEXT1_ADDR = 0x90F00000
TEXT0_SECTIONS = .isr_vector .isr_extratext .text .gc.blocks.table .data .ARM
TEXT1_SECTIONS = .cyw43_fw

# Bluetooth off, to make room for LVGL and displayif.
MICROPY_PY_BLUETOOTH = 0
MICROPY_BLUETOOTH_NIMBLE = 0

# LVGL without its 40 px font, to make room as well. A program that wants it
# can load omnipython-lvgl's montserrat_40.bin at run time.
CFLAGS += -DLV_FONT_MONTSERRAT_40=0

# The DSI host and LTDC drivers that displayif's mipidsi uses. The
# stm32h7xx_hal_conf.h beside this file enables them.
HAL_SRC_C += $(addprefix $(STM32LIB_HAL_BASE)/Src/stm32$(MCU_SERIES)xx_, hal_dsi.c hal_ltdc.c)
