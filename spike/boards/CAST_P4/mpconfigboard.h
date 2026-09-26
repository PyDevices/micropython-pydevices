// The stock ESP32_GENERIC_P4 header, unchanged.
#include "boards/ESP32_GENERIC_P4/mpconfigboard.h"

// usbif: extend TinyUSB's device configuration (descriptor + class config are
// provided by the usbif external C module). Guarded by __has_include so the
// board still builds when usbif is not in the preset: the header is on the
// include path only because usbif's micropython.cmake contributes that
// directory, so this is exactly an "is the module in this build" test. MSC is
// inside the guard too -- the class is useless without usbif, which supplies
// the block callbacks and chooses what the drive holds from Python at runtime.
// Moved here from cmods' board-header patch, 2026-09-23.
#if defined(__has_include)
#if __has_include("usbif_tusb_ext.h")
#define MICROPY_HW_USB_EXT_TUSB_CONFIG  "usbif_tusb_ext.h"
#define MICROPY_HW_USB_MSC (1)
#endif
#endif
