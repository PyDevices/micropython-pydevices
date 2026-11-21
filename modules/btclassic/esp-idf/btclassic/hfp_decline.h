// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Between hfp_hf.c and hfp_decline.c: the XAPL information that marks the
// command hfp_decline.c sends as Android's audio policy instead. Bluedroid
// refuses XAPL information that isn't exactly ESP_BT_HF_AT_SEND_XAPL_LEN (14)
// characters long.

#pragma once

#define BTC_HF_POLICY_MARKER "sink-audio-pol"
