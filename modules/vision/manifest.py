# vision: esp-vision's OpenMV-style image stack, compiled as it is
# (modules/.esp-vision). ESP32-P4 and ESP32-S3. SPIKE: image, imageio and,
# on the P4, h264; sensor, display, espdl and rtsp are not in yet.
#
# image takes and returns ulab ndarrays, so ulab comes too, by its real path
# (the spelling ../manifest.py uses, so naming ulab as well compiles it once).
import os

c_module(".")
c_module(os.path.join(os.path.realpath(os.path.join(os.getcwd(), "..", "ulab")), "code"))
