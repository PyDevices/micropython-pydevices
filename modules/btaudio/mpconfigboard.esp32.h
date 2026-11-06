// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// btaudio runs ESP-IDF's Bluedroid host for Bluetooth Classic, and ESP-IDF
// runs one host, so MicroPython's `bluetooth` module, which is built on
// NimBLE, is left out of this firmware.
#define MICROPY_PY_BLUETOOTH (0)
