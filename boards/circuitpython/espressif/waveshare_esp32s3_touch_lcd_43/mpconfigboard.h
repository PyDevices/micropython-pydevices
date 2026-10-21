// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Touch-LCD-4.3, as a generic ESP32-S3 N8R8 board.

#pragma once

#define MICROPY_HW_BOARD_NAME       "Waveshare ESP32-S3-Touch-LCD-4.3"
#define MICROPY_HW_MCU_NAME         "ESP32S3"

// The UART the CH343 bridge brings out on the "UART" USB-C connector.
#define CIRCUITPY_CONSOLE_UART_TX (&pin_GPIO43)
#define CIRCUITPY_CONSOLE_UART_RX (&pin_GPIO44)
