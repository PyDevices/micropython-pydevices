# Waveshare ESP32-P4-WIFI6-DEV-KIT: ESP32-P4 (revision 3, 16 MB flash, 32 MB
# PSRAM) with an ESP32-C6 for Wi-Fi 6 and Bluetooth. CircuitPython has no
# definition for it; this is ours, after CircuitPython's ESP32-P4X-Function-EV.
#
# The console is the P4's USB Serial/JTAG, on the USB-C marked "USB", so
# esptool flashes it with no buttons. Opening or closing that port resets the
# chip. The high-speed OTG port is wired as a host (the Type-A sockets), so
# there is no USB device and no CIRCUITPY drive over USB; files go over the
# REPL. The creator and creation IDs stand in until this board has registered
# ones. CircuitPython doesn't drive the C6 yet, so there is no Wi-Fi.
CIRCUITPY_CREATOR_ID = 0x00000000
CIRCUITPY_CREATION_ID = 0x00000000

CIRCUITPY_USB_DEVICE = 0
CIRCUITPY_ESP_USB_SERIAL_JTAG = 1

IDF_TARGET = esp32p4

CIRCUITPY_ESP_FLASH_SIZE = 16MB
CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m

CIRCUITPY_ESP_PSRAM_SIZE = 32MB
CIRCUITPY_ESP_PSRAM_MODE = hpi
CIRCUITPY_ESP_PSRAM_FREQ = 200m

CIRCUITPY_ESP32P4_REV = 3

# One 6 MB app partition and a 10 MB CIRCUITPY, in place of the 16 MB layout's
# two 2 MB OTA slots: LVGL, audiodsp and pygraphics come to 1.9 MB, which
# leaves a 2 MB slot no room. Over-the-air updates need two slots, so this
# board has none.
FLASH_SIZE_SDKCONFIG = boards/$(BOARD)/sdkconfig-flash.defaults
CIRCUITPY_STORAGE_EXTEND = 0
CIRCUITPY_DUALBANK = 0

# LVGL links its own AnimatedGIF decoder (lvgl-circuitpython's micropython.mk).
CIRCUITPY_GIFIO = 0
