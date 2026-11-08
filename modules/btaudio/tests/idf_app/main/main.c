// SPDX-FileCopyrightText: 2026 Brad Barnett
//
// SPDX-License-Identifier: MIT

// Calls every function in btaudio.h, so the linker keeps them all and a
// missing or changed ESP-IDF API fails this build.

#include "btaudio.h"
#include "nvs_flash.h"

void app_main(void) {
    nvs_flash_init();
    btaudio_start("btaudio", 0, true);
    int16_t frames[256 * 2];
    btaudio_read(frames, 256);
    btaudio_status_t st;
    btaudio_status(&st);
    char title[64];
    btaudio_text(BTAUDIO_TEXT_TITLE, title, sizeof(title));
    uint16_t l, r;
    btaudio_levels(&l, &r);
    btaudio_changes();
    btaudio_command(BTAUDIO_CMD_PLAY);
    btaudio_key(0x49, 2);
    btaudio_set_player(2, 1);
    btaudio_refresh();
    uint8_t cover[64];
    btaudio_cover(cover, btaudio_cover_size() < sizeof(cover) ? btaudio_cover_size() : sizeof(cover));
    btaudio_answer();
    btaudio_hangup();
    btaudio_dial(NULL);
    btaudio_voice(false);
    btaudio_dtmf('1');
    int16_t mic[32] = {0};
    btaudio_write_mic(mic, 32);
    btaudio_set_volume(64);
    btaudio_disconnect();
    btaudio_started();
    btaudio_stop();
}
