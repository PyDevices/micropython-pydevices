# tflite: TensorFlow Lite Micro inference, esp-vision's module compiled as it
# is (modules/.esp-vision). C only; ESP32-P4 and ESP32-S3.
#
# Model.predict() takes and returns ulab ndarrays, so ulab comes too. By its
# real path, the spelling ../manifest.py uses, so naming ulab as well compiles
# it once.
import os

c_module(".")
c_module(os.path.join(os.path.realpath(os.path.join(os.getcwd(), "..", "ulab")), "code"))
