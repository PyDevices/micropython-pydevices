// Host stand-in for esp_sccb_intf: every transfer goes to trace.c, which
// records writes and answers reads from its register image.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef struct esp_sccb_io_t *esp_sccb_io_handle_t;
esp_err_t esp_sccb_transmit_reg_a8v8(esp_sccb_io_handle_t h, uint8_t r, uint8_t v);
esp_err_t esp_sccb_transmit_reg_a16v8(esp_sccb_io_handle_t h, uint16_t r, uint8_t v);
esp_err_t esp_sccb_transmit_reg_a8v16(esp_sccb_io_handle_t h, uint8_t r, uint16_t v);
esp_err_t esp_sccb_transmit_reg_a16v16(esp_sccb_io_handle_t h, uint16_t r, uint16_t v);
esp_err_t esp_sccb_transmit_reg_a16v32(esp_sccb_io_handle_t h, uint16_t r, uint32_t v);
esp_err_t esp_sccb_transmit_reg_a16(esp_sccb_io_handle_t h, uint16_t r, const void *v, size_t n);
esp_err_t esp_sccb_transmit_receive_reg_a8v8(esp_sccb_io_handle_t h, uint8_t r, uint8_t *v);
esp_err_t esp_sccb_transmit_receive_reg_a16v8(esp_sccb_io_handle_t h, uint16_t r, uint8_t *v);
esp_err_t esp_sccb_transmit_receive_reg_a8v16(esp_sccb_io_handle_t h, uint8_t r, uint16_t *v);
esp_err_t esp_sccb_transmit_receive_reg_a16v16(esp_sccb_io_handle_t h, uint16_t r, uint16_t *v);
esp_err_t esp_sccb_transmit_receive_reg_a16v32(esp_sccb_io_handle_t h, uint16_t r, uint32_t *v);
esp_err_t esp_sccb_transmit_receive_reg_a16(esp_sccb_io_handle_t h, uint16_t r, void *v, size_t n);
esp_err_t esp_sccb_transmit_v16(esp_sccb_io_handle_t h, uint16_t v);
esp_err_t esp_sccb_receive_v16(esp_sccb_io_handle_t h, uint16_t *v);
esp_err_t esp_sccb_del_i2c_io(esp_sccb_io_handle_t h);
