# The Adafruit Feather RP2040 with room for LVGL: CircuitPython's own
# adafruit_feather_rp2040 definition, with a 3 MB firmware region in place of
# 1020 KB (the board has 8 MB of flash, so CIRCUITPY keeps about 5 MB) and
# gifio off, since LVGL links its own AnimatedGIF decoder, and the CPU at its
# rated 200 MHz.
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

# The CPU at 200 MHz, the RP2040's rated clock since pico-sdk 2.1.1, where the
# SDK's default is 125. The SDK raises the core voltage to the 1.15 V that
# rating needs before it raises the clock. The flash follows at a quarter of
# the CPU's clock: 50 MHz quad reads, inside both chips' rating.
CFLAGS += -DPICO_USE_FASTEST_SUPPORTED_CLOCK=1
