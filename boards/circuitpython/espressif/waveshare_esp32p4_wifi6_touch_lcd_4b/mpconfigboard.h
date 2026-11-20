// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-P4-WIFI6-Touch-LCD-4B.

#pragma once

#define MICROPY_HW_BOARD_NAME       "Waveshare ESP32-P4-WIFI6-Touch-LCD-4B"
#define MICROPY_HW_MCU_NAME         "ESP32P4"

#define CIRCUITPY_BOOT_BUTTON       (&pin_GPIO35)

// The console: the UART the board's CH343 bridge brings out on its "UART"
// USB-C connector.
#define CIRCUITPY_CONSOLE_UART_TX   (&pin_GPIO37)
#define CIRCUITPY_CONSOLE_UART_RX   (&pin_GPIO38)

// The touch controller, the ES8311 codec and the camera's SCCB.
#define DEFAULT_I2C_BUS_SCL         (&pin_GPIO8)
#define DEFAULT_I2C_BUS_SDA         (&pin_GPIO7)
