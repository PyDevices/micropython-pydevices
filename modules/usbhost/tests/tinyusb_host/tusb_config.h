// SPDX-License-Identifier: MIT
//
// TinyUSB's host stack, configured as the Arduino GIGA R1's USB-A port is
// (variants/stm32/ARDUINO_GIGA/DISPLAY_SHIELD/mpconfigboard.h), for
// test_tuh_enum_retry.c to build on a PC against a fake controller.
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU                OPT_MCU_NONE
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUD_ENABLED             (0)
#define CFG_TUH_ENABLED             (1)
#define CFG_TUH_MAX_SPEED           (OPT_MODE_FULL_SPEED)
#define CFG_TUH_HUB                 (1)
#define CFG_TUH_DEVICE_MAX          (4)
#define CFG_TUH_API_EDPT_XFER       (1)
#define CFG_TUH_ENUMERATION_BUFSIZE (256)

#endif
