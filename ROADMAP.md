# Roadmap

Work here mostly follows bug reports and requests, so if something is broken
or missing for you, open an
[issue](https://github.com/PyDevices/micropython-pydevices/issues).

## Firmware

- **Your own firmware from the firmware page.** A build you made, in a
  codespace or anywhere else, goes onto your board from the browser; it's
  planned on the [site's roadmap](https://github.com/PyDevices/PyDevices.github.io/blob/main/ROADMAP.md).
- **Python packages in a ROMFS partition on a 4 MB ESP32.** Put instruments or
  an app's modules in flash beside the firmware and run them in place, where a
  LittleFS filesystem has no room for that many files.
- **USB networking from the board.** Switch on `network.USBD_NCM` in the
  ESP32 builds that carry lwIP, so a computer reaches a board over the USB
  cable with no Wi-Fi.

## Watching a board

- **An RTSP server built from source.** Point VLC or a recorder at an ESP32-P4
  and watch its camera in H.264, with G.711 sound, without the prebuilt library
  the `rtsp` module uses today.
- **A live camera through Miracast.** Send the CSI camera's picture to a TV or
  Windows Wireless Display with `castif.Cast`.

## CircuitPython-compatible builds

[What builds today](docs/reference/modules.md#which-modules-build-on-circuitpython).
tflite and vision stay MicroPython-only: they are esp-vision's modules, not
ours to port.

- **castif casting on the ESP32-P4.** castif and h264enc build and encode there,
  but a cast can't start until CircuitPython drives the boards' ESP32-C6 radio.
- **More of usbhost on an ESP32-P4.** A wireless keyboard-and-mouse dongle
  works on the WIFI6 DEV-KIT; a drive, a webcam and a phone come next.
- **The USB host drivers on CircuitPython's own `usb.core`**, on the RP2040
  and i.MX: planned in [pydevices](https://github.com/PyDevices/pydevices/blob/main/ROADMAP.md#usb).

## tflite models

- **Small int8 models on the ESP32-P4 and S3:** the microWakeWord wake words,
  micro_speech, person_detection, MobileNet-v2 int8 and yamnet-cry-distill.
  Each has run on a P4; the S3 is judged from the ops, not yet run.
- **More to try:** efficientdet-lite0, silero-vad, MoveNet and the MLPerf Tiny
  models, and int8 conversions of fp16 and fp32 models such as YuNet.
- **YAMNet's full 521-class int8 model.** The published yamnet-mel-int8-tflm
  conversion scores every class near 0.5; a fresh int8 conversion would run.
- **micro_speech's own int8 audio preprocessor.** It needs TFLM's Signal
  ops, which tflite doesn't register; until then `audiofeatures` makes the
  same features.

## picotts, speech on the board

- **Speech that starts sooner.** `say()` renders a whole sentence before it
  plays, which on an ESP32-S3 is 1 to 4 s of silence first; the engine runs at
  twice real time there, so a short lead would do.
- **picotts on the ESP32-P4 touch panels.** It already speaks on the ESP32-S3
  T-Embed and the P4 WIFI6 DEV-KIT; the panels with a built-in screen are next.

## Bluetooth Classic

- **btclassic on the ESP32-S31.** Its C already builds for the S31 on ESP-IDF
  6.1; the module follows once MicroPython builds for the chip.
- **A Pico W speaker calls its sender back after a lost link,** as the
  classic ESP32 does. Today it calls back only when `AudioSink` starts.
- **Three-way calling in `HandsFree` and `AudioGateway`:** hold a call, take a
  second one and swap between them.
- **Files over OBEX FTP:** browse, get and put files on a phone or computer,
  on the OBEX code that `ObjectPush`, `PhoneBook` and `Messages` already use.
- **A board that serves its own phonebook and messages** (PBAP and MAP
  servers), so a car or headset can read them from the board.
- **A Pico W that shares its Wi-Fi over Bluetooth** (a PAN access point), and
  a dial-up networking (DUN) serial profile on `Serial`.
- **Browsing a Pico W's own library from a phone:** AVRCP target browsing
  beside `AudioSource`.

Call audio on the Pico W's hands-free side isn't planned: the CYW43439's
firmware never brings the call's audio link up.

## Module roadmaps

The modules plan their own work; each roadmap is next to its code.

- [`audioif`](modules/audioif/ROADMAP.md), the audio hardware layer
- [`cameraif`](modules/cameraif/ROADMAP.md), the MIPI-CSI camera
- [`displayif`](modules/displayif/ROADMAP.md), the display buses and drivers
- [`usbhost`](modules/usbhost/ROADMAP.md), the USB host
