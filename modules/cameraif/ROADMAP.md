# Roadmap

- `frame_size` choosing the sensor's mode when one matches it exactly, so a
  `FrameSize` the sensor can stream natively (the OV5647's `FHD`) costs no
  scaling and keeps the whole field of view.
- `GRAYSCALE` straight from the ISP's luma, instead of a pass on the PPA.
- Sensors that send YUV or RGB, such as the OV5640 and GC2145, and the
  HDMI-to-CSI bridges, streaming around the ISP. Today `Camera` finds and
  names them, then says it can't stream them.
- The sensor descriptions in `sensors/` run on a board, one at a time. Each
  file's header says whether it has run on a sensor yet; only the OV5647's and
  the OV2710's have. Four drivers with real logic still have no description.
- Tests that run in CI without a camera, beside the lint it runs today.

Bugs, and things you need that don't work yet, go to
[issues](https://github.com/PyDevices/micropython-pydevices/issues).
