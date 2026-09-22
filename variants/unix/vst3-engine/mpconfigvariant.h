// The micropython-vst3 sidecar interpreter on unix: upstream's standard
// variant plus os.dupterm. What it leaves OUT (sockets, SSL, FFI) is set in
// mpconfigvariant.mk beside this file.
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#include "variants/mpconfigvariant_common.h"
#define MICROPY_PY_OS_DUPTERM (1)
