// Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on H2. See
// mpconfigvariant.cmake beside this file.

#include "py/mpconfig.h"
#include "hal/usb_wrap_ll.h"

void boardctrl_startup(void);

void wifi6_dev_kit_h2_board_startup(void) {
    // Give the full-speed OTG controller (USB_WRAP) FSLS PHY 0, GPIO24/25,
    // which H2 is wired to, and USB Serial/JTAG PHY 1, GPIO26/27. ESP-IDF has
    // the call but never makes it, so the eFuse default (USB_PHY_SEL = 0)
    // would stand. This runs in app_main, before the MicroPython task starts
    // and so before usb_phy_init() creates the device's PHY.
    usb_wrap_ll_phy_select(&USB_WRAP, 0);
    boardctrl_startup();
}
