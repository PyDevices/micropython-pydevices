// SPDX-License-Identifier: MIT
//
// Waveshare ESP32-S3-Touch-LCD-7: wake the panel through the CH422G expander,
// then give CircuitPython the 800x480 RGB panel as board.DISPLAY, so the REPL
// shows on the glass from boot.

#include "supervisor/board.h"
#include "mpconfigboard.h"

#include "py/mphal.h"
#include "shared-bindings/board/__init__.h"
#include "shared-bindings/busio/I2C.h"
#include "shared-bindings/dotclockframebuffer/DotClockFramebuffer.h"
#include "shared-bindings/framebufferio/FramebufferDisplay.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-module/displayio/__init__.h"

// The CH422G takes each register as its own I2C address.
#define CH422G_WR_SET (0x24)  // bit 0: IO0-IO7 are outputs
#define CH422G_WR_IO  (0x38)  // IO0-IO7 output levels

// Its IO pins on this board.
#define EXIO_TP_RST  (1)
#define EXIO_LCD_BL  (2)
#define EXIO_LCD_RST (3)
#define EXIO_SD_CS   (4)
#define EXIO_USB_SEL (5)      // low: the second USB-C is the S3's USB; high: CAN

static void ch422g_write(busio_i2c_obj_t *i2c, uint8_t reg, uint8_t value) {
    common_hal_busio_i2c_write(i2c, reg, &value, 1);
}

// Backlight on, the panel out of reset, touch and the SD card deselected and
// idle, and the multiplexer on USB (the expander's own default selects CAN).
static void expander_init(void) {
    busio_i2c_obj_t i2c;
    i2c.base.type = &busio_i2c_type;
    common_hal_busio_i2c_construct(&i2c, DEFAULT_I2C_BUS_SCL, DEFAULT_I2C_BUS_SDA, 400000, 255);
    uint8_t io = 0xff & ~(1 << EXIO_USB_SEL) & ~(1 << EXIO_LCD_RST);
    ch422g_write(&i2c, CH422G_WR_SET, 0x01);
    ch422g_write(&i2c, CH422G_WR_IO, io);
    mp_hal_delay_ms(10);
    io |= 1 << EXIO_LCD_RST;
    ch422g_write(&i2c, CH422G_WR_IO, io);
    mp_hal_delay_ms(100);
    common_hal_busio_i2c_deinit(&i2c);
}

// RGB565 data lines, least significant bit first.
static const mcu_pin_obj_t *red_pins[] = {
    &pin_GPIO1, &pin_GPIO2, &pin_GPIO42, &pin_GPIO41, &pin_GPIO40
};
static const mcu_pin_obj_t *green_pins[] = {
    &pin_GPIO39, &pin_GPIO0, &pin_GPIO45, &pin_GPIO48, &pin_GPIO47, &pin_GPIO21
};
static const mcu_pin_obj_t *blue_pins[] = {
    &pin_GPIO14, &pin_GPIO38, &pin_GPIO18, &pin_GPIO17, &pin_GPIO10
};

static void display_init(void) {
    dotclockframebuffer_framebuffer_obj_t *framebuffer = &allocate_display_bus_or_raise()->dotclock;
    framebuffer->base.type = &dotclockframebuffer_framebuffer_type;

    // 14 MHz rather than the panel's 16: the scanout streams from PSRAM, and at
    // 16 MHz it takes enough of PSRAM's bandwidth that the board's Wi-Fi loses
    // packets. The same timings as board.TFT_TIMINGS.
    common_hal_dotclockframebuffer_framebuffer_construct(
        framebuffer,
        &pin_GPIO5,     // de
        &pin_GPIO3,     // vsync
        &pin_GPIO46,    // hsync
        &pin_GPIO7,     // pclk
        red_pins, MP_ARRAY_SIZE(red_pins),
        green_pins, MP_ARRAY_SIZE(green_pins),
        blue_pins, MP_ARRAY_SIZE(blue_pins),
        14000000,       // frequency
        800, 480,       // width, height
        4, 8, 8, false, // horizontal: pulse, back porch, front porch, idle low
        4, 8, 8, false, // vertical: pulse, back porch, front porch, idle low
        false,          // DE idle high
        false,          // pclk active high
        false,          // pclk idle high
        0               // overscan left
        );

    framebufferio_framebufferdisplay_obj_t *display = &allocate_display_or_raise()->framebuffer_display;
    display->base.type = &framebufferio_framebufferdisplay_type;
    common_hal_framebufferio_framebufferdisplay_construct(display, framebuffer, 0, true);
}

void board_init(void) {
    expander_init();
    display_init();
}

// Use the MP_WEAK supervisor/shared/board.c versions of routines not defined here.
