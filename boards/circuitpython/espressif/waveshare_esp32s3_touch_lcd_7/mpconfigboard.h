// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Touch-LCD-7.

#pragma once

#define MICROPY_HW_BOARD_NAME       "Waveshare ESP32-S3-Touch-LCD-7"
#define MICROPY_HW_MCU_NAME         "ESP32S3"

// The CH422G expander (backlight, panel and touch resets, SD card select,
// the USB/CAN multiplexer) and the GT911 touch controller share this bus.
#define DEFAULT_I2C_BUS_SDA (&pin_GPIO8)
#define DEFAULT_I2C_BUS_SCL (&pin_GPIO9)

// The micro SD card; its chip select is the CH422G's EXIO4.
#define DEFAULT_SPI_BUS_MOSI (&pin_GPIO11)
#define DEFAULT_SPI_BUS_SCK (&pin_GPIO12)
#define DEFAULT_SPI_BUS_MISO (&pin_GPIO13)

// The UART the CH343 bridge brings out on the "UART" USB-C connector.
#define CIRCUITPY_CONSOLE_UART_TX (&pin_GPIO43)
#define CIRCUITPY_CONSOLE_UART_RX (&pin_GPIO44)
