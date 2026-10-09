# Waveshare ESP32-S3-Touch-LCD-7: ESP32-S3 (8 MB flash, 8 MB octal PSRAM), an
# 800x480 RGB panel (ST7262) and a GT911 touch controller. CircuitPython has
# no definition for it; this is ours.
#
# The console is the UART on GPIO43/44, which the board's CH343 bridge brings
# out on its "UART" USB-C connector, so esptool flashes it with no buttons.
# The S3's own USB goes to the second connector through a multiplexer the
# CH422G expander drives (shared with CAN), and is left off here: native USB
# needs a USB VID and PID of its own, and the creator and creation IDs below
# stand in until this board has registered ones.
CIRCUITPY_CREATOR_ID = 0x00000000
CIRCUITPY_CREATION_ID = 0x00000000

CIRCUITPY_USB_DEVICE = 0
CIRCUITPY_ESP_USB_SERIAL_JTAG = 0
UF2_BOOTLOADER = 0

IDF_TARGET = esp32s3

CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m
CIRCUITPY_ESP_FLASH_SIZE = 8MB

CIRCUITPY_ESP_PSRAM_SIZE = 8MB
CIRCUITPY_ESP_PSRAM_MODE = opi
CIRCUITPY_ESP_PSRAM_FREQ = 80m

CIRCUITPY_DOTCLOCKFRAMEBUFFER = 1

# One 4 MB app partition and a 4 MB CIRCUITPY, in place of the 8 MB layout's
# two 2 MB OTA slots: LVGL alone is about 1 MB, and with it the firmware no
# longer fits 2 MB. Over-the-air updates need two slots, so this board has none.
FLASH_SIZE_SDKCONFIG = boards/$(BOARD)/sdkconfig-flash.defaults
# Neither CIRCUITPY nor an update can use a second app slot that isn't there.
CIRCUITPY_STORAGE_EXTEND = 0
CIRCUITPY_DUALBANK = 0

# LVGL links its own AnimatedGIF decoder (lvgl-circuitpython's micropython.mk).
CIRCUITPY_GIFIO = 0
