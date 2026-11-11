// Host stand-in for FreeRTOS, just enough for esp_cam_sensor's drivers to
// compile on a PC under trace.c. Delays are recorded, not slept.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef void *TimerHandle_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void (*TimerCallbackFunction_t)(TimerHandle_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY 0xffffffffu
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define IRAM_ATTR
void trace_delay_ms(uint32_t ms);
static inline void vTaskDelay(TickType_t t) { trace_delay_ms(t); }
TimerHandle_t xTimerCreate(const char *n, TickType_t p, UBaseType_t r, void *id, TimerCallbackFunction_t cb);
void *pvTimerGetTimerID(TimerHandle_t t);
BaseType_t xTimerStart(TimerHandle_t t, TickType_t w);
BaseType_t xTimerStop(TimerHandle_t t, TickType_t w);
BaseType_t xTimerDelete(TimerHandle_t t, TickType_t w);
