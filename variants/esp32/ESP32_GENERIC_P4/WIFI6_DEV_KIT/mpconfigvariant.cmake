# Waveshare ESP32-P4-WIFI6-DEV-KIT: upstream's C6_WIFI variant (a revision 3
# P4 with an ESP32-C6 for Wi-Fi and BLE), with TinyUSB's device on the
# full-speed controller instead of the high-speed one.
#
# The kit wires the high-speed controller only to its USB-A sockets, which
# source 5 V, so it can't be a device port there. As a USB host it can, and
# usbif's host runs on it. The full-speed controller's pins, GPIO26 (D-) and
# GPIO27 (D+), reach header P6 at pins 32 and 37 and nothing else, so a USB-C
# socket wired there (5.1 kOhm from each CC pin to GND, VBUS left open) is the
# device port, and the board is host and device at once. The REPL is on the
# CH343 UART ("USB TO UART") and on USB Serial/JTAG ("USB") as before, and on
# the full-speed device's CDC once that socket is wired.
#
# The USB setting is in the mpconfigboard.h beside this file, not here: the
# TinyUSB component reads the board header but not MICROPY_DEF_BOARD.
include(${MICROPY_BOARD_DIR}/mpconfigvariant_C6_WIFI.cmake)

list(FILTER MICROPY_DEF_BOARD EXCLUDE REGEX "^MICROPY_HW_BOARD_NAME=")
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Waveshare ESP32-P4-WIFI6-DEV-KIT"
)
