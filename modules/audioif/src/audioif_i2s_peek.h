// SPDX-License-Identifier: MIT
// The I2S driver's ring and its writer's position: see audioif_i2s_peek.c.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_types.h"

// The ring's buffers, how many, and the bytes in each. False before the
// channel has its DMA.
bool audioif_i2s_peek_ring(i2s_chan_handle_t tx, uint8_t ***bufs,
    uint32_t *count, uint32_t *bytes);

// The buffer the writer is filling and how far into it it has written.
void audioif_i2s_peek_writer(i2s_chan_handle_t tx, void **buf,
    uint32_t *offset);
