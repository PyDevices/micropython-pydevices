// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT
//
// A small cache sync doesn't queue behind a large one (esp32, chips whose
// internal RAM is behind a cache: the ESP32-P4).
//
// With CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS, esp_cache_msync() holds one
// mutex for the whole of every sync, so that a large one can be written back
// a chunk at a time with interrupts on between chunks. i2s_channel_write()
// syncs each block it copies into its DMA buffer, a few hundred bytes, and so
// waits for that mutex. Its holder can be a display driver writing back a
// whole frame, tens of milliseconds, or a low-priority task that has been
// switched out: the audio pump was seen waiting 48 ms in
// s_acquire_mutex_from_task_context() and the DAC ran dry.
//
// A sync no longer than one chunk is one critical section whichever way it
// is done, and needs nothing kept apart from it. So the link wraps
// esp_cache_msync (-Wl,--wrap in micropython.cmake) and does those here,
// under the same spinlock ESP-IDF uses, without the mutex. Everything else
// goes to ESP-IDF's function unchanged.

#include "sdkconfig.h"

#if defined(CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS) && CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS

#include <stdbool.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_err.h"
#include "esp_private/esp_cache_private.h"
#include "freertos/FreeRTOS.h"
#include "hal/cache_hal.h"
#include "soc/soc_caps.h"

esp_err_t __real_esp_cache_msync(void *addr, size_t size, int flags);

esp_err_t __wrap_esp_cache_msync(void *addr, size_t size, int flags) {
    #if SOC_CACHE_WRITEBACK_SUPPORTED
    // Cache to memory, data, no invalidate after: what a DMA write needs.
    const int other = ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST | ESP_CACHE_MSYNC_FLAG_INVALIDATE;
    if (addr != NULL && size != 0 && size <= CONFIG_ESP_MM_CACHE_MSYNC_C2M_CHUNKED_OPS_MAX_LEN
        && (flags & other) == 0 && (uint32_t)addr + size > (uint32_t)addr && xPortCanYield()) {
        uint32_t level = 0, id = 0;
        bool ok = cache_hal_vaddr_to_cache_level_id((uint32_t)addr, size, &level, &id);
        if (ok && (flags & ESP_CACHE_MSYNC_FLAG_UNALIGNED) == 0) {
            // ESP-IDF refuses a misaligned sync that doesn't say it is one: let it.
            const uint32_t line = cache_hal_get_cache_line_size(level, CACHE_TYPE_DATA);
            ok = line != 0 && (uint32_t)addr % line == 0 && size % line == 0;
        }
        if (ok) {
            esp_cache_sync_ops_enter_critical_section();
            bool valid = cache_hal_writeback_addr((uint32_t)addr, size);
            esp_cache_sync_ops_exit_critical_section();
            if (valid) {
                return ESP_OK;
            }
        }
    }
    #endif
    return __real_esp_cache_msync(addr, size, flags);
}

#endif
