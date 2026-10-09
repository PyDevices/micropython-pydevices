// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-P4-WIFI6-DEV-KIT.

#pragma once

#define MICROPY_HW_BOARD_NAME       "Waveshare ESP32-P4-WIFI6-DEV-KIT"
#define MICROPY_HW_MCU_NAME         "ESP32P4"

#define CIRCUITPY_BOOT_BUTTON       (&pin_GPIO35)

// The UART the CH343 bridge brings out on the "USB TO UART" connector.
#define DEFAULT_UART_BUS_RX         (&pin_GPIO38)
#define DEFAULT_UART_BUS_TX         (&pin_GPIO37)

// The camera's SCCB, the ES8311 codec and the display connector's I2C.
#define DEFAULT_I2C_BUS_SCL         (&pin_GPIO8)
#define DEFAULT_I2C_BUS_SDA         (&pin_GPIO7)
