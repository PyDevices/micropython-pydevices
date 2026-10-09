// Waveshare ESP32-P4-WIFI6-DEV-KIT: included before ESP32_GENERIC_P4's own
// header. See mpconfigvariant.cmake beside this file for why.

// TinyUSB's device on the full-speed controller (GPIO26/27, header P6 pins 32
// and 37), leaving the high-speed one to usbif's host. This needs usbif's
// patch 0005, which gives the device the full-speed PHY: build with usbif
// (or "all").
#define MICROPY_HW_USB_HS (0)
