// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// The BTstack backend's hooks on MicroPython: BTstack comes up through the
// `bluetooth` module's own start (so BLE and Classic share one stack and one
// radio), and the link keys live in a small file on the board's filesystem.

#include "py/runtime.h"
#include "py/builtin.h"
#include "py/stream.h"
#include "extmod/modbluetooth.h"

#include "btc_btstack.h"

#define STORE_PATH "/.btclassic"

static bool started_here;

int btc_btstack_host_start(void) {
    if (mp_bluetooth_is_active()) {
        started_here = false;
        return BTC_OK;
    }
    int err = mp_bluetooth_init();
    if (err) {
        return BTC_ERR_FAIL;
    }
    started_here = true;
    return BTC_OK;
}

bool btc_btstack_host_active(void) {
    return mp_bluetooth_is_active();
}

void btc_btstack_host_stop(void) {
    // BLE may be in use too: only take the stack down if Classic brought it up.
    if (started_here) {
        mp_bluetooth_deinit();
        started_here = false;
    }
}

// The interpreter's heap, through its tracked allocations: the GC sees them
// as live until they're freed, and a soft reset wipes them with the heap.
void *btc_btstack_alloc(size_t size) {
    return m_tracked_calloc(1, size);
}

void btc_btstack_free(void *ptr) {
    if (ptr) {
        m_tracked_free(ptr);
    }
}

static mp_obj_t open_store(qstr mode) {
    mp_obj_t args[2] = { mp_obj_new_str(STORE_PATH, sizeof(STORE_PATH) - 1), MP_OBJ_NEW_QSTR(mode) };
    return mp_call_function_n_kw(MP_OBJ_FROM_PTR(&mp_builtin_open_obj), 2, 0, args);
}

void btc_btstack_store_save(const void *data, size_t len) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t f = open_store(MP_QSTR_wb);
        int err;
        mp_stream_write_exactly(f, data, len, &err);
        mp_stream_close(f);
        nlr_pop();
    } else {
        btc_log("couldn't save the link keys");
    }
}

size_t btc_btstack_store_load(void *data, size_t size) {
    size_t n = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t f = open_store(MP_QSTR_rb);
        int err;
        n = mp_stream_read_exactly(f, data, size, &err);
        mp_stream_close(f);
        nlr_pop();
    }
    return n;
}
