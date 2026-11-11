#pragma once
#include <stdint.h>
typedef struct { int model; uint32_t features; uint16_t revision; uint8_t cores; } esp_chip_info_t;
static inline void esp_chip_info(esp_chip_info_t *i) { i->model = 18; i->features = 0; i->revision = 100; i->cores = 2; }
