# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Play a computer's audio through the board's speaker over USB.

The computer sees a speaker ("Speakers (Espressif Device)" on Windows, a USB
audio output on macOS and Linux) and plays to it at 48 kHz, stereo, 16-bit.
The samples go from the USB interrupt straight into audiodsp's pump, which
plays them on the board's I2S codec, so Python never touches the audio. The
loop here only keeps the clock feedback right and follows the computer's
volume and mute. The computer's slider runs from -50 dB to 0 dB, and moves the
board's volume linearly between silence and ``VOLUME``, the board's own
listening level (85 % on the ESP32-P4 panel; 100 overdrives its amplifier).
Success is hearing the computer's audio from the board.

Needs: a board with an I2S codec, whose board config sets ``pcm_out``. The
firmware needs machine.USBDevice's streaming endpoints, audiodsp and audioif,
and the board needs micropython-lib's ``usb-device`` and usbhost's
``usb-device-audio`` package:

    build_mp.py --modules pydevices,usbhost,audiodsp,audioif

Run it:

    mpftp run examples/usb_soundcard.py

The board's serial port stays up beside the speaker. Ctrl-C stops the card.
Another board can be the computer: run ``usb_speaker.py`` on it.

Tested on: an ESP32-P4 panel with an ES8311 codec.
"""

import time

import audiopump
import usb.device
from audiodev import AudioFormat, pump
from boarddev import pcm_out
from usb.device.audio import SpeakerInterface

# Change these to match your setup.
VOLUME = 85
FORMAT = AudioFormat(48000, 2, 16)


def _transport():
    transport = pcm_out(FORMAT)
    while transport is not None and getattr(transport, "wire", None) is None:
        transport = getattr(transport, "_inner", None)
    if transport is None:
        raise RuntimeError("this board's pcm_out has no I2S wire for the pump to play on")
    return transport


def _percent(spk):
    if spk.mute:
        return 0
    db = max(-50 * 256, min(0, spk.volume))
    return round(VOLUME * (1 + db / (50 * 256)))


def main(block_frames=480, blocks=8):
    # 8 blocks of 10 ms: the feedback holds the ring near half, 40 ms.
    ring = audiopump.Ring(sample_rate=FORMAT.rate, channel_count=FORMAT.channels,
                          frames=block_frames, capacity=blocks)
    spk = SpeakerInterface(sink=ring)
    usb.device.get().init(spk, builtin_driver=True)

    transport = _transport()
    driver = pump.BusioDriver(transport.wire, FORMAT, power=getattr(transport, "audio_power", None),
                              volume=VOLUME, transport=transport)
    if not pump.owner().play(spk, ring, driver=driver, raises=True):
        raise RuntimeError(pump.owner().fault())
    print("sound card up: play to it from the computer (Ctrl-C stops)")
    level = None
    try:
        while True:
            spk.service()
            now = _percent(spk)
            if now != level:
                transport.set_volume(now)
                level = now
            time.sleep_ms(5)
    finally:
        pump.owner().stop(spk)
        usb.device.get().init(builtin_driver=True)


main()
