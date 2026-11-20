# Waveshare ESP32-S31-Touch-LCD-4.3C: an ESP32-S31-WROOM-3 (16 MB flash, 16 MB
# octal PSRAM, which every S31 build turns on: variants/esp32/sdkconfig.esp32s31)
# with an 800x480 RGB panel and a GT911 touch controller. The REPL is on the
# CH343 UART behind the "UART" USB-C connector. The S31's high-speed USB
# reaches only header J3, and the native USB device is off on every S31 build
# for now (patches/micropython-s31/0003).
list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Waveshare ESP32-S31-Touch-LCD-4.3C"
)
