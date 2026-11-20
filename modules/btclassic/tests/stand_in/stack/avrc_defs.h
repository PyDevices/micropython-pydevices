// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

#pragma once
#include <stdint.h>

#define AVRC_OP_VENDOR 0x00
#define AVRC_RSP_CHANGED 13
#define AVRC_RSP_INTERIM 15
#define AVRC_RSP_ACCEPT 9
#define AVRC_PDU_REGISTER_NOTIFICATION 0x31
#define AVRC_PDU_GET_PLAY_STATUS 0x30
#define AVRC_EVT_PLAY_STATUS_CHANGE 0x01
#define AVRC_EVT_APP_SETTING_CHANGE 0x08

typedef uint8_t tAVRC_STS;

typedef struct {
    uint8_t ctype;
    uint8_t subunit_type;
    uint8_t subunit_id;
    uint8_t opcode;
} tAVRC_HDR;

typedef struct {
    tAVRC_HDR hdr;
    uint32_t company_id;
    uint8_t *p_vendor_data;
    uint16_t vendor_len;
} tAVRC_MSG_VENDOR;

typedef union {
    tAVRC_HDR hdr;
    tAVRC_MSG_VENDOR vendor;
} tAVRC_MSG;

typedef struct {
    uint8_t pdu;
} tAVRC_RESPONSE;
