# btclassic profiles

What `btclassic` implements on each chip, and what it can't. The
[README](../README.md) shows how to use each object.

## What works

| Profile | Object | ESP32 | Pico W, Pico 2 W |
|---|---|---|---|
| A2DP sink, with AVRCP controller: track details, cover art, volume, play control, player settings | `AudioSink` | Yes | Yes |
| A2DP source, with AVRCP target: the receiver's keys, track and status | `AudioSource` | Yes | Yes |
| HFP hands-free unit | `HandsFree` | Yes, with the call's audio | Yes, call control only |
| HFP audio gateway | `AudioGateway` | Yes, with the call's audio | Yes, call control only |
| SPP serial port | `Serial` | Yes | Yes |
| A raw L2CAP channel | `L2CAP` | Yes | Yes |
| HID device: keyboard, mouse, gamepad, your own descriptor | `HIDDevice` | Yes | Yes |
| HID host | `HIDHost` | Yes | Yes |
| PBAP client: the phone's contacts and call lists | `PhoneBook` | Yes | Yes |
| MAP client: texts, with notifications | `Messages` | Yes | Yes |
| OPP: send and receive files and contact cards | `ObjectPush` | Yes | Yes |
| SDP: publish a service, list a peer's | `Service`, `Bluetooth.services()` | Yes | Yes |
| AVRCP browsing: a phone's media library | `Library` | No | Yes |
| PAN: the internet through a phone | `Tether` | No | Yes |
| Sniff mode and sub-rating | `Bluetooth.sniff()`, `subrate()` | No | Yes |

The A2DP codec is SBC. A board runs one A2DP role at a time, `AudioSink` or
`AudioSource`, one of `HandsFree` and `AudioGateway`, and one of `HIDDevice`
and `HIDHost`.

## Not supported, and why

| What | Where | Because |
|---|---|---|
| Call audio over HFP | Pico W, Pico 2 W | The CYW43439's Bluetooth firmware accepts the audio link but never completes it ([raspberrypi/pico-sdk#1461](https://github.com/raspberrypi/pico-sdk/issues/1461)), so `HandsFree.audio` stays False |
| AVRCP browsing | ESP32 | Bluedroid in ESP-IDF 5.5.4 has no browsing commands in its AVRCP API |
| PAN | ESP32 | Bluedroid in ESP-IDF 5.5.4 has no PAN or BNEP API |
| Requesting sniff mode | ESP32 | Bluedroid decides when a link sniffs and has no call to ask for it; `sniff()` and `subrate()` raise `OSError` |
| AAC | ESP32 | A2DP AAC needs ESP-IDF 6.1; the build uses 5.5.4 |
| The shortest accepted encryption key | Pico W, Pico 2 W | The controller refuses the command, so `config(min_key_size=)` is ESP32 only |
| Bluetooth Classic | ESP32-S2, S3, C3, C6, H2, P4 | These chips have no BR/EDR radio. The classic ESP32 does |
| `btclassic` | Ports other than ESP32 (the classic chip) and rp2 | It's written against Bluedroid and BTstack, and builds only for those. A unix build runs it on a loopback backend for tests |
