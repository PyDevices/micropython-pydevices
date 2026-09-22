// The PyDevices unix build: upstream's standard variant (its feature level and
// the common unix features, included from the port tree) plus os.dupterm,
// which the desktop display backends use to mirror the REPL into their window.
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)
#include "variants/mpconfigvariant_common.h"
#define MICROPY_PY_OS_DUPTERM (1)
