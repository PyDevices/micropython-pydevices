// Waveshare ESP32-S31-Zero: included before ESP32_GENERIC_S31's own header.
// The USB-C connector is USB Serial/JTAG, not the native USB device.
#define MICROPY_HW_ENABLE_USBDEV              (0)
#define MICROPY_HW_ESP_USB_SERIAL_JTAG        (1)
