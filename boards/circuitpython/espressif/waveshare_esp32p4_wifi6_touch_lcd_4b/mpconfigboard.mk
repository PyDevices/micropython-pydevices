# Waveshare ESP32-P4-WIFI6-Touch-LCD-4B: ESP32-P4 (revision 1, 32 MB flash,
# 32 MB PSRAM) with a 720x720 MIPI-DSI panel and an ESP32-C6 for Wi-Fi 6 and
# Bluetooth. CircuitPython has no definition for it; this is ours.
#
# The console is the UART on GPIO37/38, which the board's CH343 brings out on
# its "UART" USB-C, so esptool flashes it with no buttons. The other USB-C
# reaches only the high-speed OTG controller and is left off here: a USB
# device needs a VID and PID of its own, and the creator and creation IDs
# below stand in until this board has registered ones. CircuitPython doesn't
# drive the C6 yet, so there is no Wi-Fi, and the panel isn't set up yet.
CIRCUITPY_CREATOR_ID = 0x00000000
CIRCUITPY_CREATION_ID = 0x00000000

CIRCUITPY_USB_DEVICE = 0
CIRCUITPY_ESP_USB_SERIAL_JTAG = 0

IDF_TARGET = esp32p4

CIRCUITPY_ESP_FLASH_SIZE = 16MB
CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m

CIRCUITPY_ESP_PSRAM_SIZE = 32MB
CIRCUITPY_ESP_PSRAM_MODE = hpi
CIRCUITPY_ESP_PSRAM_FREQ = 200m

# One 6 MB app partition and a 10 MB CIRCUITPY in the first 16 MB of the
# flash, in place of the 16 MB layout's two 2 MB OTA slots:
# LVGL, audiodsp and pygraphics come to 1.9 MB, which leaves a 2 MB slot
# no room. Over-the-air updates need two slots, so this board has none.
FLASH_SIZE_SDKCONFIG = boards/$(BOARD)/sdkconfig-flash.defaults
CIRCUITPY_STORAGE_EXTEND = 0
CIRCUITPY_DUALBANK = 0

# LVGL links its own AnimatedGIF decoder (lvgl-circuitpython's micropython.mk).
CIRCUITPY_GIFIO = 0
