// The mpvst sidecar interpreter on windows: upstream's dev variant with no
// sockets, no SSL and no FFI (see the unix variant for why).
//
// Each is switched off here, BEFORE the dev header and the port header are
// read, because on windows the C side decides and the make side only follows:
// the overlay's mpconfigport.h turns MICROPY_PY_SOCKET and MICROPY_PY_FFI on
// under #ifndef, and the dev header does the same for SSL and mbedtls. The
// make-level overrides in mpconfigvariant.mk keep the sources and libraries
// (modsocket's Winsock calls, libffi, mbedtls) out of the link to match.
// A make-level switch alone is not enough: MICROPY_PY_SOCKET=0 on the make
// line left socket in the engine from 0.3.0 until this header said it too.
#define MICROPY_PY_SOCKET (0)
#define MICROPY_PY_FFI (0)
#define MICROPY_PY_SSL (0)
#define MICROPY_SSL_MBEDTLS (0)
#include "variants/dev/mpconfigvariant.h"
