// SPDX-License-Identifier: MIT
//
// Where the I2S driver's writer is: the one buffer and offset that
// `i2s_channel_write()` will write into next. The underrun count in
// _audioif.c needs it to know where on the wire the audio just written will
// play, and the driver keeps it private (`i2s_dma_t.curr_ptr` and `rw_pos`).
// This file is the only one that includes the private header, so a driver
// that renames those fields fails to compile here, by name, rather than
// counting wrong somewhere else. ESP-IDF 5.5 and 6.1 both have them.

#include "audioif_i2s_peek.h"

#include "i2s_private.h"

bool audioif_i2s_peek_ring(i2s_chan_handle_t tx, uint8_t ***bufs,
    uint32_t *count, uint32_t *bytes) {
    if (tx == NULL || tx->dma.bufs == NULL) {
        return false;
    }
    *bufs = tx->dma.bufs;
    *count = tx->dma.desc_num;
    *bytes = tx->dma.buf_size;
    return true;
}

// Called on the writer's own task right after `i2s_channel_write()` returns,
// so nothing else is moving these two fields.
void audioif_i2s_peek_writer(i2s_chan_handle_t tx, void **buf,
    uint32_t *offset) {
    *buf = tx->dma.curr_ptr;
    *offset = tx->dma.rw_pos;
}
