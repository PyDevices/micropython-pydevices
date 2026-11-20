// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Stand-ins for the few Bluedroid names avrcp_settings.c uses, so
// test_settings.c builds it on the desktop. Layouts follow ESP-IDF 6.1's
// stack/avrc_defs.h.

#pragma once
#include <stdbool.h>
#include <stdint.h>
