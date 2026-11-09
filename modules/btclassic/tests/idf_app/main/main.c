// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Calls every function in btclassic.h, so the linker keeps them all and a
// missing or changed ESP-IDF API fails this build.

#include "btclassic.h"
#include "nvs_flash.h"

void app_main(void) {
    nvs_flash_init();

    // The radio.
    btc_radio_set_name("btclassic");
    btc_radio_set_pin("0000");
    btc_radio_set_visibility(true, true);
    btc_radio_start();
    char text[64];
    btc_radio_name(text, sizeof(text));
    btc_radio_pin(text, sizeof(text));
    bool discoverable, connectable;
    btc_radio_visibility(&discoverable, &connectable);
    btc_addr_t mac;
    btc_radio_mac(mac);
    btc_addr_t paired[4];
    btc_radio_paired(paired, 4);
    btc_radio_changes();
    btc_radio_set_class(0);
    btc_radio_class();

    // The sink.
    btc_sink_start(24576, true);
    int16_t frames[256 * 2];
    btc_sink_read(frames, 256);
    btc_sink_changes();
    btc_sink_status_t sink;
    btc_sink_status(&sink);
    btc_ring_stats_t stats;
    btc_sink_stats(&stats);
    btc_sink_text(BTC_SINK_TITLE, text, sizeof(text));
    uint8_t cover[64];
    btc_sink_cover(cover, btc_sink_cover_size() < sizeof(cover) ? btc_sink_cover_size() : sizeof(cover));
    uint16_t l, r;
    btc_sink_levels(&l, &r);
    btc_sink_key(0x44, 2);
    btc_sink_set_player(2, 1);
    btc_sink_refresh();
    btc_sink_set_volume(64);
    btc_sink_connect(NULL);

    // The hands-free unit.
    btc_hf_start(400, 2);
    btc_hf_read(frames, 256);
    int16_t mic[32] = {0};
    btc_hf_write(mic, 32);
    btc_hf_changes();
    btc_hf_status_t hf;
    btc_hf_status(&hf);
    btc_hf_stats(&stats);
    btc_hf_text(BTC_HF_OPERATOR, text, sizeof(text));
    btc_hf_answer();
    btc_hf_hangup();
    btc_hf_dial(NULL);
    btc_hf_dtmf('1');
    btc_hf_voice_recognition(false);
    btc_hf_set_speaker_gain(10);
    btc_hf_set_mic_gain(10);
    btc_hf_connect_audio();
    btc_hf_disconnect_audio();
    btc_hf_connect(NULL);
    btc_hf_disconnect();
    btc_hf_started();
    btc_hf_stop();

    btc_sink_disconnect();
    btc_sink_started();
    btc_sink_stop();

    // The source, after the sink: one A2DP role at a time.
    btc_radio_inquiry(5120);
    btc_radio_inquiring();
    btc_found_t found[BTC_FOUND_MAX];
    btc_radio_found(found, BTC_FOUND_MAX);
    btc_radio_inquiry(0);
    btc_source_start(4096);
    btc_source_connect(NULL);
    btc_source_stream(true);
    btc_source_write(mic, 16, 48000, 1);
    btc_source_changes();
    btc_source_status_t src;
    btc_source_status(&src);
    btc_source_stats(&stats);
    btc_source_text(BTC_SOURCE_TITLE, text, sizeof(text));
    btc_source_set_volume(64);
    btc_source_key();
    btc_source_set_track("title", "artist", "album", "genre", 1, 10, 180000);
    btc_source_set_play_status(1, 0);
    btc_source_stream(false);
    btc_source_disconnect();
    btc_source_started();
    btc_source_stop();

    btc_spp_start("Serial Port", 4096);
    btc_spp_connect(NULL, 0);
    btc_spp_any();
    uint8_t bytes[16];
    btc_spp_read(bytes, sizeof(bytes));
    btc_spp_write(bytes, sizeof(bytes));
    btc_spp_write_room();
    btc_spp_changes();
    btc_spp_status_t spp;
    btc_spp_status(&spp);
    btc_spp_connected();
    btc_spp_disconnect();
    btc_spp_started();
    btc_spp_stop();

    static const uint8_t mouse[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0xc0};
    btc_hid_device_set_ids(0x1209, 0x0001, 0x0100);
    btc_hid_device_start(mouse, sizeof(mouse), 0x80, "Mouse", true);
    btc_hid_device_connect(NULL);
    btc_hid_device_send(bytes, 4);
    btc_hid_device_output(bytes, sizeof(bytes));
    btc_hid_device_set_feature(bytes, 2);
    btc_hid_device_feature(0, bytes, sizeof(bytes));
    btc_hid_stop();
    btc_hid_host_start();
    btc_hid_host_connect(NULL, true);
    btc_hid_host_read(bytes, sizeof(bytes));
    btc_hid_host_send(1, bytes, 1);
    btc_hid_host_descriptor(bytes, sizeof(bytes));
    uint16_t vendor, product, version;
    btc_hid_host_ids(&vendor, &product, &version);
    btc_hid_host_get_report(BTC_HID_FEATURE, 0);
    btc_hid_host_got_report(bytes, sizeof(bytes));
    btc_hid_host_set_report(BTC_HID_FEATURE, bytes, 1);
    btc_hid_host_set_done();
    btc_hid_status_t hid;
    btc_hid_status(&hid);
    btc_hid_changes();
    btc_hid_disconnect();
    btc_hid_stop();

    btc_pb_start(65536);
    btc_pb_connect(NULL);
    btc_pb_pull("telecom/pb.vcf", 0, BTC_PB_ALL, 0, BTC_PB_PROP_FN | BTC_PB_PROP_TEL);
    btc_pb_list("telecom/pb", 0, 10, 0, "555", 1);
    btc_pb_entry("telecom/pb", "1.vcf", 1, 0);
    btc_pb_data(bytes, sizeof(bytes));
    btc_pb_release();
    btc_pb_status_t pb;
    btc_pb_status(&pb);
    btc_pb_changes();
    btc_pb_started();
    btc_pb_disconnect();
    btc_pb_stop();

    btc_msg_start(65536);
    btc_msg_connect(NULL, -1);
    btc_msg_folders("telecom/msg", 0, 64);
    btc_msg_list("telecom/msg", "inbox", 0, 10, BTC_MSG_UNREAD, BTC_MSG_NO_EMAIL, 0, 32);
    btc_msg_get("0001", false, true);
    btc_msg_set_status("0001", BTC_MSG_STATUS_READ, true);
    btc_msg_push("telecom/msg", "draft", bytes, sizeof(bytes), true, true);
    btc_msg_update_inbox();
    btc_msg_notify(true);
    btc_msg_data(bytes, sizeof(bytes));
    btc_msg_release();
    btc_msg_event(bytes, sizeof(bytes));
    btc_msg_status_t msg;
    btc_msg_status(&msg);
    btc_msg_changes();
    btc_msg_started();
    btc_msg_disconnect();
    btc_msg_stop();

    btc_hf_stop();
    btc_ag_start(400);
    btc_ag_connect(NULL);
    btc_ag_set_indicators(1, 4, 0, 3);
    btc_ag_set_text(BTC_AG_OPERATOR, "Bench");
    btc_ag_ring("+15550100");
    btc_ag_answer();
    btc_ag_connect_audio();
    btc_ag_write(mic, 32);
    btc_ag_read(frames, 256, 1);
    btc_ag_dial("123");
    btc_ag_alerting();
    btc_ag_hangup();
    btc_ag_set_in_band_ring(true);
    btc_ag_voice_recognition(false);
    btc_ag_set_speaker_gain(9);
    btc_ag_set_mic_gain(9);
    btc_ag_dtmf(text, sizeof(text));
    btc_ag_text(BTC_AG_CALLER, text, sizeof(text));
    btc_ag_status_t ag;
    btc_ag_status(&ag);
    btc_ag_stats(&stats);
    btc_ag_changes();
    btc_ag_started();
    btc_ag_disconnect_audio();
    btc_ag_disconnect();
    btc_ag_stop();
    btc_radio_unpair(NULL);
    btc_radio_active();
    btc_radio_stop();
}
