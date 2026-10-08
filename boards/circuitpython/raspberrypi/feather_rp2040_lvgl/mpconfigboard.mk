# The Adafruit Feather RP2040 with room for LVGL: CircuitPython's own
# adafruit_feather_rp2040 definition, with a 3 MB firmware region in place of
# 1020 KB (the board has 8 MB of flash, so CIRCUITPY keeps about 5 MB) and
# gifio off, since LVGL links its own AnimatedGIF decoder.
USB_VID = 0x239A
USB_PID = 0x80F2
USB_PRODUCT = "Feather RP2040"
USB_MANUFACTURER = "Adafruit"

CHIP_VARIANT = RP2040
CHIP_FAMILY = rp2

EXTERNAL_FLASH_DEVICES = "GD25Q64C,W25Q64JVxQ"

CIRCUITPY_GIFIO = 0

# Must match link.ld.
CFLAGS += -DCIRCUITPY_FIRMWARE_SIZE='(3072 * 1024)'
