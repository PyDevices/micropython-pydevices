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

// USB host on the USB-A socket, for usbhost's TinyUSB engine: TinyUSB's host
// stack on OTG_HS (root port 1) through its internal full-speed PHY, beside
// the device on USB-C (root port 0). Root port 1 gets no mode, so boot starts
// only the device, and usb.core.start() starts the host. No DMA: the M7's
// data cache would need every buffer cleaned and invalidated.
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE)
#define CFG_TUH_ENABLED             (1)
#define CFG_TUH_MAX_SPEED           (OPT_MODE_FULL_SPEED)
#define CFG_TUH_DWC2_SLAVE_ENABLE   (1)
#define CFG_TUH_DWC2_DMA_ENABLE     (0)
#define CFG_TUH_HUB                 (1)
#define CFG_TUH_DEVICE_MAX          (4)
#define CFG_TUH_API_EDPT_XFER       (1)
#define CFG_TUH_ENUMERATION_BUFSIZE (1024)
#define USBHOST_TUH_RHPORT          (1)
#define MICROPY_BOARD_PENDSV_ENTRIES PENDSV_DISPATCH_USBHOST,

// The socket's 5 V switch: PA15 high is on. Its reset state is JTDI with a
// pull-up, which turns the socket on, so early init drives it low
// (usb_a_power.c), and only a running host turns it on.
#define USBHOST_TUH_VBUS_PIN        (pin_A15)
#undef MICROPY_BOARD_EARLY_INIT
#define MICROPY_BOARD_EARLY_INIT    GIGA_DISPLAY_SHIELD_early_init
void GIGA_DISPLAY_SHIELD_early_init(void);
