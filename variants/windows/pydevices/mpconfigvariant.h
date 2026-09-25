// The PyDevices windows build: upstream's dev variant, included from the port
// tree, plus an 8 MB GC heap by default (upstream's is 2 MB on 64-bit);
// -X heapsize= still overrides it. Needs patch 0012.
#include "variants/dev/mpconfigvariant.h"
#define MICROPY_UNIX_DEFAULT_HEAP_SIZE (8 * 1024 * 1024)
// Run a built-in module's __init__ on import, as unix does. The windows port
// sits at the core ROM level, where this is off, so `import jpegio` after
// lv.init() never registered displayif's LVGL decoder (displayif#41).
#define MICROPY_MODULE_BUILTIN_INIT (1)
