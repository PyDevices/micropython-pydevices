// Included ahead of every file the tracer compiles: what ESP-IDF's build
// would have put in scope by itself.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/param.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#ifndef BIT
#define BIT(n) (1UL << (n))
#endif
