# vision: esp-vision's OpenMV-style image stack, compiled as it is
# (modules/.esp-vision). C only; ESP32-P4 and ESP32-S3. image, imageio,
# espdl, and on the P4 h264 and rtsp; not sensor or display (see
# docs/esp-vision.md).
#
# image works with ulab ndarrays: name ulab with it, or use all (a module
# never brings another).
c_module(".")
