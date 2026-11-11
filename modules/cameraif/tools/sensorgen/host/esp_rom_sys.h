#pragma once
#include <stdint.h>
void trace_delay_ms(uint32_t ms);
static inline void esp_rom_delay_us(uint32_t us) { trace_delay_ms((us + 999) / 1000); }
