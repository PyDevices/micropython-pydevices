// The ESP32-P4's hardware JPEG engine, behind jpegio (jpegio_hw.c). On every
// other chip JPEGIO_HW is 0 and the two functions are stubs that say no.
#ifndef JPEGIO_HW_H
#define JPEGIO_HW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "py/obj.h"

#ifndef JPEGIO_HW
#define JPEGIO_HW 0
#endif

// Both are always declared and always linked: off the P4 they are stubs that
// say no. The callers test JPEGIO_HW as a plain constant, never with #if,
// because the CMake ports' QSTR pass does not see this module's compile
// definitions, and an error message only inside an #if it misjudges goes
// missing from the compressed-message table.

// A bytes object, or MP_OBJ_NULL when the engine could not be had or refused
// the image (the caller then encodes in software, or raises).
mp_obj_t jpegio_hw_encode(const void *pixels, int width, int height, size_t stride,
    int format, bool swap, int quality, bool subsample, bool exact);

// Decodes a whole JPEG to RGB565 at (x, y) of a target `stride` pixels wide.
// Returns NULL, or a short reason the engine could not.
const char *jpegio_hw_decode(const uint8_t *data, size_t len, int width, int height,
    uint16_t *pixels, size_t stride, int x, int y);

#endif
