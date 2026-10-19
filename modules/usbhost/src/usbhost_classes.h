// SPDX-License-Identifier: MIT
//
// Class bitmask, mirroring the names in the portable Python API. A bitmask
// rather than strings so an event record stays fixed-size and ISR-safe; the
// translation to names happens on the consumer side, where allocation is
// allowed. Shared between the module surface (mod_usbhost.c) and the host
// engine (usbhost_host.c), which derives the bits from interface descriptors.
#ifndef USBHOST_CLASSES_H
#define USBHOST_CLASSES_H

#define USBHOST_CLASS_HID  (1u << 0)
#define USBHOST_CLASS_MSC  (1u << 1)
#define USBHOST_CLASS_CDC  (1u << 2)
#define USBHOST_CLASS_MIDI (1u << 3)
#define USBHOST_CLASS_UAC  (1u << 4)
#define USBHOST_CLASS_UVC  (1u << 5)
// Any other interface class (vendor-specific, printer, ...): only usb.core
// drives these, through the generic transfers.
#define USBHOST_CLASS_OTHER (1u << 6)

#endif // USBHOST_CLASSES_H
