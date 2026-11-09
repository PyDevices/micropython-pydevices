# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Make the board a speakerphone for your phone's calls.

The board follows a paired phone as a car kit does. It prints the phone's
battery and signal, and each call with the caller's number, and answers an
incoming call by itself. While a call's audio is on the board, the audio
pump plays the caller through the speaker and sends the microphone back to
the phone, all in C.

Needs: a classic ESP32 board with PSRAM, firmware built with the btclassic
and audiodsp modules, and a board config with audio_out and pcm_in roles on
one I2S port, such as the ESP32 Audio Kit's.

Run it:

    mpftp run btclassic_speakerphone.py

Pair your phone with the board's hostname, then call someone, or have
someone call you. Their voice comes out of the board's speaker.

Tested on: ESP32 Audio Kit (ESP32-A1S), MicroPython 1.30.0-preview, with a
Galaxy S21.
"""

import time

import board_peripherals
import btclassic
import network
from audiodev import AudioFormat

# Change these to match your setup.
ANSWER = True  # answer incoming calls by themselves
LINE_IN = False  # True sends the line-in jack, on a board that has one

# Wi-Fi and Bluetooth share the ESP32's radio. Turn Wi-Fi off for calls.
network.WLAN(network.STA_IF).active(False)

# A phone links the hands-free unit when it connects for music, so start
# both. Calls come as stereo frames here, the shape the speaker plays.
bt = btclassic.Bluetooth()
music = btclassic.AudioSink(bt)
phone = btclassic.HandsFree(bt, channels=2)
print("Pair with %s, then make a call." % bt.config("name"))

speaker = mic = None
while True:
    changed = phone.changes()

    # Report the phone and its calls as they change.
    if changed & btclassic.PHONE:
        print("Phone: battery %s/5, signal %s/5" % (phone.battery, phone.signal))
    if changed & btclassic.CALL:
        print("Call:", phone.call, phone.caller)
        if phone.call == "incoming" and ANSWER:
            phone.answer()

    # When the call's audio comes to the board, play it, and send the
    # microphone from the same pull. The microphone opens after the
    # speaker, because it runs on the speaker's clock.
    if phone.audio and speaker is None:
        rate = phone.sample_rate
        speaker = board_peripherals.audio_out(AudioFormat(rate, 2, 16))
        speaker.play(phone)
        if LINE_IN:
            mic = board_peripherals.pcm_in(AudioFormat(rate, 1, 16), input=board_peripherals.LINE)
        else:
            mic = board_peripherals.pcm_in(AudioFormat(rate, 1, 16))
        mic.open()
        phone.uplink(mic)
        print("Call audio on the board at", rate, "Hz")

    # When the audio goes back to the phone, or the call ends, let go.
    if not phone.audio and speaker is not None:
        phone.uplink(None)
        mic.close()
        speaker.stop()
        speaker.close()
        speaker = mic = None
        print("Call audio off the board; sent", phone.stats()["sent"], "frames")
    time.sleep_ms(100)
