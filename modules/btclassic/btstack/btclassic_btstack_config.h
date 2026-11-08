// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// BTstack's configuration for a build with btclassic: MicroPython's own
// (extmod/btstack/btstack_config_common.h and the rp2 port's two transport
// settings), so the `bluetooth` module keeps working, plus Bluetooth Classic
// and room for its profiles. The port takes this header in place of its own
// through MICROPY_BLUETOOTH_BTSTACK_CONFIG_FILE.

#ifndef BTCLASSIC_BTSTACK_CONFIG_H
#define BTCLASSIC_BTSTACK_CONFIG_H

// The rp2 port's CYW43 transport.
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4

// BLE, as MicroPython configures it.
#define ENABLE_BLE
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_CENTRAL
#define ENABLE_L2CAP_LE_CREDIT_BASED_FLOW_CONTROL_MODE
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LOG_ERROR

// Bluetooth Classic and the profiles btclassic uses.
#define ENABLE_CLASSIC
#define ENABLE_SCO_OVER_HCI
#define ENABLE_L2CAP_ENHANCED_RETRANSMISSION_MODE
#define ENABLE_GOEP_L2CAP
#define ENABLE_HFP_WIDE_BAND_SPEECH
#define ENABLE_AVRCP_COVER_ART

// Buffers. An A2DP media packet from a phone is up to about 1 KB.
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_HOST_ACL_PACKET_LEN HCI_ACL_PAYLOAD_SIZE
#define HCI_HOST_ACL_PACKET_NUM 3
#define HCI_HOST_SCO_PACKET_LEN 120
#define HCI_HOST_SCO_PACKET_NUM 3

// Static pools: BTstack is given no malloc.
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_HCI_CONNECTIONS 2
#define MAX_NR_L2CAP_SERVICES 10
#define MAX_NR_L2CAP_CHANNELS 12
#define MAX_NR_RFCOMM_MULTIPLEXERS 1
#define MAX_NR_RFCOMM_SERVICES 3
#define MAX_NR_RFCOMM_CHANNELS 3
#define MAX_NR_BTSTACK_LINK_KEY_DB_MEMORY_ENTRIES 8
#define MAX_NR_BNEP_SERVICES 1
#define MAX_NR_BNEP_CHANNELS 1
#define MAX_NR_HFP_CONNECTIONS 1
#define MAX_NR_WHITELIST_ENTRIES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_SERVICE_RECORD_ITEMS 8
#define MAX_NR_AVDTP_STREAM_ENDPOINTS 1
#define MAX_NR_AVDTP_CONNECTIONS 1
#define MAX_NR_AVRCP_CONNECTIONS 2
#define MAX_NR_AVRCP_BROWSING_CONNECTIONS 1
#define MAX_NR_GOEP_SERVER_SERVICES 1
#define MAX_NR_GOEP_SERVER_CONNECTIONS 1
#define MAX_NR_HID_HOST_CONNECTIONS 1
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4

// A fixed-size ATT database, as MicroPython has it.
#define MAX_ATT_DB_SIZE 512

#define HAVE_EMBEDDED_TIME_MS
#define HCI_RESET_RESEND_TIMEOUT_MS 1000

#endif // BTCLASSIC_BTSTACK_CONFIG_H
