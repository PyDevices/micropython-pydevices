# Roadmap

- `frame_size` choosing the sensor's mode when one matches it exactly, so a
  `FrameSize` the sensor can stream natively (the OV5647's `FHD`) costs no
  scaling and keeps the whole field of view.
- `GRAYSCALE` straight from the ISP's luma, instead of a pass on the PPA.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
