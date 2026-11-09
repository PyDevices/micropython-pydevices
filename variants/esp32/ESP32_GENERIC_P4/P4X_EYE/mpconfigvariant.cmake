# Espressif ESP32-P4X-EYE: upstream's C6_WIFI variant (a revision 3 P4 with an
# ESP32-C6 for Wi-Fi and BLE over ESP-Hosted), named for the board. The board's
# own settings (the C6's SDIO pins, the OV2710 camera, the partition table) are
# in the sdkconfig and partitions.csv beside this file; the USB and stack
# settings are in mpconfigboard.h, which the TinyUSB component reads too.
include(${MICROPY_BOARD_DIR}/mpconfigvariant_C6_WIFI.cmake)

list(FILTER MICROPY_DEF_BOARD EXCLUDE REGEX "^MICROPY_HW_BOARD_NAME=")
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Espressif ESP32-P4X-EYE"
)
