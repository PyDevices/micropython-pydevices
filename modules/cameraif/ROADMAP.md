# Roadmap

- `frame_size` choosing the sensor's mode when one matches it exactly, so a
  `FrameSize` the sensor can stream natively (the OV5647's `FHD`) costs no
  scaling and keeps the whole field of view.
- `GRAYSCALE` straight from the ISP's luma, instead of a pass on the PPA.
- Tests that run in CI without a camera, beside the lint it runs today.
- A sensor driver written as a Python class, beside the `sensor=` dict, so a
  camera whose setup is code rather than a register table (Adafruit's
  CircuitPython drivers, for one) runs with a thin adapter.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
