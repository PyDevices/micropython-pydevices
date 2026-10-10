# Waveshare ESP32-P4-WIFI6-DEV-KIT with TinyUSB's device on the USB-C marked
# "USB" (H2): the WIFI6_DEV_KIT variant beside this one, plus a PHY swap.
#
# H2 is wired to GPIO24 (D-) and GPIO25 (D+), the P4's first full-speed PHY,
# which the chip gives to USB Serial/JTAG out of reset. The full-speed OTG
# controller, where WIFI6_DEV_KIT puts TinyUSB, gets the second PHY on GPIO26
# and GPIO27, and on this board those reach only header P6. This variant swaps
# the two at boot, before the PHY is created (board_init.c beside this file),
# so the device comes out on H2 with no wiring at all. USB Serial/JTAG moves
# to P6 pins 32 (D-) and 37 (D+) in exchange.
#
# The REPL stays on the CH343 UART ("USB TO UART", H1), and on the device's
# CDC through H2 whenever the device wears CDC. The high-speed controller
# still reaches only the USB-A sockets, for usbif's host, so the board is host
# and device at once.
include(${CMAKE_CURRENT_LIST_DIR}/../WIFI6_DEV_KIT/mpconfigvariant.cmake)

list(FILTER MICROPY_DEF_BOARD EXCLUDE REGEX "^MICROPY_HW_BOARD_NAME=")
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on H2"
)

list(APPEND MICROPY_SOURCE_BOARD
    ${CMAKE_CURRENT_LIST_DIR}/board_init.c
)
