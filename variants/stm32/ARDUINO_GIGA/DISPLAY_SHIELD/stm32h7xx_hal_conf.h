// Arduino GIGA R1 WiFi with the GIGA Display Shield: included after
// ARDUINO_GIGA's own HAL configuration. displayif's mipidsi drives the
// shield's panel through the DSI host and the LTDC.

#ifndef PYDEVICES_GIGA_DISPLAY_SHIELD_HAL_CONF_H
#define PYDEVICES_GIGA_DISPLAY_SHIELD_HAL_CONF_H

#define HAL_DSI_MODULE_ENABLED
#include "stm32h7xx_hal_dsi.h"
#include "stm32h7xx_hal_ltdc.h"

#endif
