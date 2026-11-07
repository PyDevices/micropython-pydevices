// Arduino GIGA R1 WiFi with the GIGA Display Shield: the board's early init,
// then the USB-A socket's power off. PA15 drives the socket's 5 V switch, and
// out of reset it's JTDI with a pull-up, which turns the socket on. It stays
// off until a program starts the USB host (usb.core.start()).

#include "py/mphal.h"

void GIGA_DISPLAY_SHIELD_early_init(void) {
    GIGA_board_early_init();
    mp_hal_pin_output(pin_A15);
    mp_hal_pin_low(pin_A15);
}
