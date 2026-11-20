# Waveshare ESP32-S31-Zero: a bare ESP32-S31 with 32 MB of flash (a GD25Q256E,
# sdkconfig beside this file) and 16 MB of octal PSRAM. Its USB-C connector is
# wired to USB Serial/JTAG, so the REPL is there (mpconfigboard.h beside this
# file); the high-speed USB pins reach only pads.
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Waveshare ESP32-S31-Zero"
)
