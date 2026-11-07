// Arduino GIGA R1 WiFi with the GIGA Display Shield: included after
// ARDUINO_GIGA's own header. See mpconfigvariant.mk beside this file.

#undef MICROPY_HW_BOARD_NAME
#define MICROPY_HW_BOARD_NAME "Arduino GIGA R1 WiFi with GIGA Display Shield"

// The Wi-Fi chip's firmware goes in its own section, which the linker places
// in QSPI flash (cyw43_qspi.ld) and the build writes out as firmware1.bin.
#define CYW43_RESOURCE_ATTRIBUTE __attribute__((section(".cyw43_fw"), aligned(4)))

// USB through TinyUSB rather than ST's stack, for machine.USBDevice: with it,
// a program can make the USB-C port a MIDI device (or any other class) at
// run time. The REPL's CDC port is TinyUSB's own.
#define MICROPY_HW_TINYUSB_STACK (1)
