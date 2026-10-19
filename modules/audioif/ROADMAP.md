# Roadmap

What's planned for audioif. Bugs, and things you need that don't work yet, go
to [issues](https://github.com/PyDevices/micropython-pydevices/issues).

## Playback

- Stereo on a one-speaker board: when the board says its amplifier picks one
  slot rather than mixing, `I2SOut` plays `(L + R) / 2` so neither channel is
  lost. Boards state how their wire reaches their speaker.

## Capture

- `audioi2sin.I2SIn` as an audiosample, so `I2SOut.play(mic)` and a mixer can
  take a live microphone the way CircuitPython 11's can.
- `audiobusio.PDMIn` for PDM microphones, on the I2S peripheral's PDM mode.
- A one-channel read from a multi-mic codec returns the average of the
  enabled mics, with a way to ask for a single mic; boards state which mics
  are live.
