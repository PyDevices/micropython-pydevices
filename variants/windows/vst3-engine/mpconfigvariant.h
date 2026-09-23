// The micropython-vst3 sidecar interpreter on windows: upstream's dev variant
// without SSL. The dev header turns SSL and mbedtls on under #ifndef, so they
// are switched off here BEFORE it is included; sockets and FFI are make-level
// switches and are set in mpconfigvariant.mk.
#define MICROPY_PY_SSL (0)
#define MICROPY_SSL_MBEDTLS (0)
#include "variants/dev/mpconfigvariant.h"
