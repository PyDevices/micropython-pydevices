# LILYGO T-Watch S3 (ESP32-S3, 16 MB flash, 8 MB octal PSRAM) with one large
# app partition. The rest is CircuitPython's own lilygo_twatch_s3 board,
# copied unchanged: board.c, pins.c, mpconfigboard.h and the frozen libraries.
#
# CircuitPython's 16 MB layout has two 2 MB OTA slots and a UF2 bootloader.
# With LVGL (about 1 MB) beside audiodsp and pygraphics, the firmware no
# longer fits 2 MB, so this layout has one 4 MB app partition and a 12 MB
# CIRCUITPY instead, and no UF2 bootloader: flash it with esptool. Over-the-air
# updates need two slots, so this board has none.
USB_VID = 0x303A
USB_PID = 0x821C

USB_PRODUCT = "T-Watch-S3"
USB_MANUFACTURER = "LILYGO"

IDF_TARGET = esp32s3

CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m
CIRCUITPY_ESP_FLASH_SIZE = 16MB

CIRCUITPY_ESP_PSRAM_SIZE = 8MB
CIRCUITPY_ESP_PSRAM_MODE = opi
CIRCUITPY_ESP_PSRAM_FREQ = 80m

UF2_BOOTLOADER = 0
FLASH_SIZE_SDKCONFIG = boards/$(BOARD)/sdkconfig-flash.defaults
# Neither CIRCUITPY nor an update can use a second app slot that isn't there.
CIRCUITPY_STORAGE_EXTEND = 0
CIRCUITPY_DUALBANK = 0

# LVGL links its own AnimatedGIF decoder (lvgl-circuitpython's micropython.mk).
CIRCUITPY_GIFIO = 0

# Specialized board.
CIRCUITPY_ESPCAMERA = 0
CIRCUITPY_PARALLELDISPLAYBUS = 0
CIRCUITPY_MAX3421E = 0
CIRCUITPY_CANIO = 0
CIRCUITPY_COUNTIO = 0
CIRCUITPY_PS2IO = 0
CIRCUITPY_RGBMATRIX = 0
CIRCUITPY_ROTARYIO = 0

# Include these Python libraries in firmware.
FROZEN_MPY_DIRS += $(TOP)/frozen/Adafruit_CircuitPython_FocalTouch
FROZEN_MPY_DIRS += $(TOP)/frozen/Adafruit_CircuitPython_IRRemote
FROZEN_MPY_DIRS += $(TOP)/frozen/Adafruit_CircuitPython_Register
FROZEN_MPY_DIRS += $(TOP)/frozen/Adafruit_CircuitPython_DRV2605
FROZEN_MPY_DIRS += $(TOP)/frozen/Adafruit_CircuitPython_PCF8563
FROZEN_MPY_DIRS += $(TOP)/frozen/CircuitPython_AXP2101
FROZEN_MPY_DIRS += $(TOP)/frozen/CircuitPython_BMA423
