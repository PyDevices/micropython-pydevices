# Roadmap

What's planned for audioif. Bugs, and things you need that don't work yet, go
to [issues](https://github.com/PyDevices/omnipython-builds/issues).

## Playback

- Stereo on a one-speaker board: when the board says its amplifier picks one
  slot rather than mixing, `I2SOut` plays `(L + R) / 2` so neither channel is
  lost. Boards state how their wire reaches their speaker.

## Capture

- `audiobusio.PDMIn` on an ESP32, through its I2S peripheral's PDM mode, as
  an STM32H7 already records one through DFSDM.

## Examples

- Audio from one board to another over I2S, with no network: a sender that
  plays through `I2SOut`, and a receiver that records it with `I2SIn`. They
  take three wires and a ground, and either end can be any I2S device.
