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
    btc_hid_device_start(mouse, sizeof(mouse), 0x80, "Mouse", true);
    btc_hid_device_connect(NULL);
    btc_hid_device_send(bytes, 4);
    btc_hid_device_output(bytes, sizeof(bytes));
    btc_hid_stop();
    btc_hid_host_start();
    btc_hid_host_connect(NULL, true);
    btc_hid_host_read(bytes, sizeof(bytes));
    btc_hid_host_send(1, bytes, 1);
    btc_hid_host_descriptor(bytes, sizeof(bytes));
    btc_hid_status_t hid;
    btc_hid_status(&hid);
    btc_hid_changes();
    btc_hid_disconnect();
    btc_hid_stop();
    btc_radio_unpair(NULL);
    btc_radio_active();
    btc_radio_stop();
}
