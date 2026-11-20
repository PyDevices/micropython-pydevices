// Forced into ESP-IDF's components/bt/common/osi/semaphore.c and mutex.c on
// the ESP32-S31 (osi_psram.cmake): Bluedroid's semaphores and mutexes are
// made in PSRAM there, and freed to match.
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

#undef xSemaphoreCreateCounting
#define xSemaphoreCreateCounting(max, initial) \
    xSemaphoreCreateCountingWithCaps((max), (initial), MALLOC_CAP_SPIRAM)
#undef xSemaphoreCreateMutex
#define xSemaphoreCreateMutex() xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM)
#undef xSemaphoreCreateRecursiveMutex
#define xSemaphoreCreateRecursiveMutex() xSemaphoreCreateRecursiveMutexWithCaps(MALLOC_CAP_SPIRAM)
#undef vSemaphoreDelete
#define vSemaphoreDelete(sem) vSemaphoreDeleteWithCaps(sem)
