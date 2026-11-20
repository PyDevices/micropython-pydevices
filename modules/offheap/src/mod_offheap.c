// offheap: byte buffers that live outside MicroPython's garbage-collected heap.
//
//   import offheap
//   frame = offheap.Buffer(800 * 480 * 2)   # zeroed, like bytearray(n)
//   fb = FrameBuffer(frame, 800, 480, RGB565)
//   ...
//   frame.deinit()                          # or let the program end
//
// MicroPython's collector is conservative: it reads every word of every
// reachable bytearray, in case one is a pointer. A few megabytes of pixels
// therefore make every collection take hundreds of milliseconds. A Buffer is
// a small heap object whose bytes come from the port's own allocator, so the
// collector sees a few words and stops: on ESP32 that is PSRAM through
// heap_caps (internal RAM on a board without it), and elsewhere C's malloc.
//
// The bytes go back on deinit(), when the Buffer is collected (its finaliser),
// and at a soft reset. For the last, every live allocation is on a list kept
// outside the heap, and a guard object held in a root pointer frees the list
// from its finaliser: a soft reset runs gc_sweep_all(), which finalises every
// object, rooted or not, while an ordinary collection never finalises the
// guard. This is how usbhost and machine.Timer shut down on a soft reset.
//
// A memoryview of a Buffer, or anything else that takes its address through
// the buffer protocol, doesn't keep the Buffer alive: the collector can't see
// a pointer into memory it doesn't manage. Keep the Buffer itself.
//
// SPDX-License-Identifier: MIT

#include <stdlib.h>
#include <string.h>

#include "py/gc.h"
#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/runtime.h"

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#endif

// One live allocation. Nodes and bytes are both outside the GC heap.
typedef struct _offheap_node_t {
    struct _offheap_node_t *next;
    void *owner;                // the Buffer object it belongs to
    uint8_t *data;
    size_t len;
} offheap_node_t;

typedef struct _offheap_buffer_obj_t {
    mp_obj_base_t base;
    offheap_node_t *node;       // NULL once the bytes are freed
} offheap_buffer_obj_t;

static offheap_node_t *offheap_live;
static size_t offheap_live_bytes;

// --- the port's allocator ---------------------------------------------------

#if defined(ESP_PLATFORM)
// Cache-line aligned, so a driver that hands the bytes to DMA gets an address
// it accepts.
#define OFFHEAP_ALIGN (64)
#if CONFIG_SPIRAM
#define OFFHEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define OFFHEAP_CAPS (MALLOC_CAP_8BIT)
#endif

static uint8_t *offheap_bytes_alloc(size_t len) {
    // heap_caps refuses a size of 0.
    return heap_caps_aligned_calloc(OFFHEAP_ALIGN, 1, len ? len : 1, OFFHEAP_CAPS);
}

static void offheap_bytes_free(uint8_t *p) {
    heap_caps_free(p);
}
#else
static uint8_t *offheap_bytes_alloc(size_t len) {
    return calloc(1, len ? len : 1);
}

static void offheap_bytes_free(uint8_t *p) {
    free(p);
}
#endif

// --- the live list ----------------------------------------------------------

// Take node off the live list and free it with its bytes. False when it isn't
// on the list: a soft reset already freed it.
static bool offheap_node_release(offheap_node_t *node, void *owner) {
    for (offheap_node_t **p = &offheap_live; *p != NULL; p = &(*p)->next) {
        if (*p == node && node->owner == owner) {
            *p = node->next;
            offheap_live_bytes -= node->len;
            offheap_bytes_free(node->data);
            free(node);
            return true;
        }
    }
    return false;
}

static void offheap_release_all(void) {
    offheap_node_t *node = offheap_live;
    offheap_live = NULL;
    offheap_live_bytes = 0;
    while (node != NULL) {
        offheap_node_t *next = node->next;
        offheap_bytes_free(node->data);
        free(node);
        node = next;
    }
}

// --- the soft-reset guard ---------------------------------------------------

MP_REGISTER_ROOT_POINTER(mp_obj_t offheap_soft_reset_guard);

static mp_obj_t offheap_guard_del(mp_obj_t self_in) {
    (void)self_in;
    offheap_release_all();
    // The guard goes with the heap being swept. Clearing the root lets the
    // next program's first Buffer make a live one.
    MP_STATE_VM(offheap_soft_reset_guard) = MP_OBJ_NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(offheap_guard_del_obj, offheap_guard_del);

static const mp_rom_map_elem_t offheap_guard_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&offheap_guard_del_obj) },
};
static MP_DEFINE_CONST_DICT(offheap_guard_locals, offheap_guard_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    offheap_guard_type,
    MP_QSTR_SoftResetGuard,
    MP_TYPE_FLAG_NONE,
    locals_dict, &offheap_guard_locals
    );

static void offheap_arm_soft_reset(void) {
    if (MP_STATE_VM(offheap_soft_reset_guard) == MP_OBJ_NULL) {
        mp_obj_base_t *guard = mp_obj_malloc_with_finaliser(mp_obj_base_t, &offheap_guard_type);
        MP_STATE_VM(offheap_soft_reset_guard) = MP_OBJ_FROM_PTR(guard);
    }
}

// --- Buffer -----------------------------------------------------------------

static const mp_obj_type_t offheap_buffer_type;

static offheap_node_t *offheap_buffer_node(mp_obj_t self_in) {
    offheap_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->node == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer has been deinitialised"));
    }
    return self->node;
}

static mp_obj_t offheap_buffer_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    mp_int_t len = mp_obj_get_int(args[0]);
    if (len < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("size must be 0 or more"));
    }
    offheap_arm_soft_reset();
    offheap_buffer_obj_t *self = mp_obj_malloc_with_finaliser(offheap_buffer_obj_t, type);
    self->node = NULL;

    uint8_t *data = offheap_bytes_alloc(len);
    if (data == NULL) {
        // Buffers nobody holds still own their bytes until their finalisers
        // run, the way bytearray's allocator collects before it gives up.
        gc_collect();
        data = offheap_bytes_alloc(len);
    }
    offheap_node_t *node = data == NULL ? NULL : malloc(sizeof(offheap_node_t));
    if (node == NULL) {
        if (data != NULL) {
            offheap_bytes_free(data);
        }
        mp_raise_msg_varg(&mp_type_MemoryError,
            MP_ERROR_TEXT("can't allocate %u bytes outside the heap"), (uint)len);
    }
    node->owner = self;
    node->data = data;
    node->len = len;
    node->next = offheap_live;
    offheap_live = node;
    offheap_live_bytes += len;
    self->node = node;
    return MP_OBJ_FROM_PTR(self);
}

// deinit(): give the bytes back now. Safe to call more than once.
static mp_obj_t offheap_buffer_deinit(mp_obj_t self_in) {
    offheap_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->node != NULL) {
        offheap_node_release(self->node, self);
        self->node = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(offheap_buffer_deinit_obj, offheap_buffer_deinit);

static mp_obj_t offheap_buffer_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return offheap_buffer_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(offheap_buffer_exit_obj, 4, 4, offheap_buffer_exit);

static mp_obj_t offheap_buffer_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    switch (op) {
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(offheap_buffer_node(self_in)->len);
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(offheap_buffer_node(self_in)->len != 0);
        default:
            return MP_OBJ_NULL;
    }
}

static mp_int_t offheap_buffer_get_buffer(mp_obj_t self_in, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    (void)flags;
    offheap_node_t *node = offheap_buffer_node(self_in);
    bufinfo->buf = node->data;
    bufinfo->len = node->len;
    bufinfo->typecode = 'B';
    return 0;
}

static void offheap_buffer_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    offheap_buffer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->node == NULL) {
        mp_printf(print, "<Buffer deinitialised>");
    } else {
        mp_printf(print, "<Buffer %u bytes>", (uint)self->node->len);
    }
}

static const mp_rom_map_elem_t offheap_buffer_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&offheap_buffer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&offheap_buffer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&offheap_buffer_exit_obj) },
};
static MP_DEFINE_CONST_DICT(offheap_buffer_locals, offheap_buffer_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    offheap_buffer_type,
    MP_QSTR_Buffer,
    MP_TYPE_FLAG_NONE,
    make_new, offheap_buffer_make_new,
    print, offheap_buffer_print,
    unary_op, offheap_buffer_unary_op,
    buffer, offheap_buffer_get_buffer,
    locals_dict, &offheap_buffer_locals
    );

// --- module -----------------------------------------------------------------

// mem_alloc(): bytes held by live Buffers, the counterpart of gc.mem_alloc().
static mp_obj_t offheap_mem_alloc(void) {
    return mp_obj_new_int_from_uint(offheap_live_bytes);
}
static MP_DEFINE_CONST_FUN_OBJ_0(offheap_mem_alloc_obj, offheap_mem_alloc);

// offheap.__revision__: which source this firmware was built from. The build
// passes OFFHEAP_REVISION (git describe); a build with no git says "unknown".
#ifndef OFFHEAP_REVISION
#define OFFHEAP_REVISION "unknown"
#endif
static const MP_DEFINE_STR_OBJ(offheap_revision_obj, OFFHEAP_REVISION);

static const mp_rom_map_elem_t offheap_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_offheap) },
    { MP_ROM_QSTR(MP_QSTR___revision__), MP_ROM_PTR(&offheap_revision_obj) },
    { MP_ROM_QSTR(MP_QSTR_Buffer), MP_ROM_PTR(&offheap_buffer_type) },
    { MP_ROM_QSTR(MP_QSTR_mem_alloc), MP_ROM_PTR(&offheap_mem_alloc_obj) },
};
static MP_DEFINE_CONST_DICT(offheap_module_globals, offheap_module_globals_table);

const mp_obj_module_t offheap_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&offheap_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_offheap, offheap_user_cmodule);
