// Espressif ESP32-P4X-EYE: included before ESP32_GENERIC_P4's own header.

// The USB-C marked "USB 2.0" is the P4's high-speed OTG controller, the
// factory firmware's mass-storage port. TinyUSB's device stays on it (the
// upstream default, MICROPY_HW_USB_HS 1). The REPL is on the USB-C marked
// "Debug", the USB Serial/JTAG port, so an app can drop the CDC and present
// the SD card alone on the other port.

// The main task's stack, as on the WIFI6_DEV_KIT: LVGL's timer pass runs
// Python callbacks that build whole screens, which outgrew the 16 KB default.
#define MICROPY_TASK_STACK_SIZE (32 * 1024)

// The microSD slot's pins at ESP-IDF's full drive strength (3) ring enough
// that about one 4 KB write in 5,000 fails. At the chip's default strength
// (2) 200 MB copies write with no errors.
#define MICROPY_HW_SDMMC_DRIVE_STRENGTH (2)

// Ethernet over USB: network.USBD_NCM, beside the REPL's serial port and the
// SD card's drive. The high-speed device has IN endpoints 1 to 7, and NCM takes
// two of them from the built-in set.
#define MICROPY_PY_NETWORK_USBD_NCM (1)
