# Roadmap

What's planned for audioif. Bugs, and things you need that don't work yet, go
to [issues](https://github.com/PyDevices/audioif/issues).

## Playback

- Stereo on a one-speaker board: when the board says its amplifier picks one
  slot rather than mixing, `I2SOut` plays `(L + R) / 2` so neither channel is
  lost. Boards state how their wire reaches their speaker.

## Capture

- `audioi2sin.I2SIn`, CircuitPython's I2S microphone class (`record()` into a
  buffer), so CircuitPython capture code runs unchanged, including a
  receive-only I2S open.
- A one-channel read from a multi-mic codec returns the average of the
  enabled mics, with a way to ask for a single mic; boards state which mics
  are live.
