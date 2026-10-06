// jpegio's C API, for other native modules that have pixels to encode.
//
// A module that may be built without jpegio declares jpegio_encode weak and
// checks it for NULL before calling, so it still links when jpegio is absent:
//
//   extern mp_obj_t jpegio_encode(...) __attribute__((weak));
//
// cameraif's capture_jpeg() does exactly that.
#ifndef JPEGIO_H
#define JPEGIO_H

#include <stdbool.h>
#include <stddef.h>

#include "py/obj.h"

// encode()'s format= values, also jpegio.RGB565 and jpegio.GRAY.
#define JPEGIO_FORMAT_RGB565 0
#define JPEGIO_FORMAT_GRAY 1

// Encodes width x height pixels, rows `stride` bytes apart, to a JPEG bytes
// object. RGB565 is native byte order unless swap. quality is 1..100;
// subsample picks 4:2:0 over 4:4:4. hardware: -1 uses the chip's JPEG engine
// when it has one, 0 never, 1 insists (OSError where there is none). exact
// false lets the ESP32-P4's engine read RGB565 itself, from the buffer when it
// can: several times faster on a large frame, but the engine widens by
// zero-filling, so full red and blue come out 7 levels dark (248, not 255);
// exact widens in software first, as a display does. *used_hw, if given, says
// which ran. Raises ValueError on bad arguments.
mp_obj_t jpegio_encode(const void *pixels, size_t len, int width, int height, size_t stride,
    int format, bool swap, int quality, bool subsample, int hardware, bool exact, bool *used_hw);

#endif
