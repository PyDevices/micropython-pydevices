// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Between avrcp_settings.c, which reads the sender's player settings out of
// Bluedroid, and avrcp_ct.c, which keeps them.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// The attribute/value pairs of AVRCP notification 0x08: every setting the
// player has when `complete` (the INTERIM reply), else those that changed.
// Called on Bluedroid's task.
void btc_avrcp_settings(const uint8_t *pairs, int count, bool complete);
