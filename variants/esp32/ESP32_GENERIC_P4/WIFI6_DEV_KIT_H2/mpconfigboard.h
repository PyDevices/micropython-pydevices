// Waveshare ESP32-P4-WIFI6-DEV-KIT, USB device on H2: included before
// ESP32_GENERIC_P4's own header. See mpconfigvariant.cmake beside this file.

// TinyUSB's device on the full-speed controller, as in WIFI6_DEV_KIT.
#include "../WIFI6_DEV_KIT/mpconfigboard.h"

// Swap the full-speed PHYs before anything creates one (board_init.c).
#define MICROPY_BOARD_STARTUP wifi6_dev_kit_h2_board_startup
void wifi6_dev_kit_h2_board_startup(void);
