// The PyDevices windows build: upstream's dev variant, included from the port
// tree, plus an 8 MB GC heap by default (upstream's is 2 MB on 64-bit);
// -X heapsize= still overrides it. Needs patch 0012.
#include "variants/dev/mpconfigvariant.h"
#define MICROPY_UNIX_DEFAULT_HEAP_SIZE (8 * 1024 * 1024)
