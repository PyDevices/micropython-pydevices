# tflite: TensorFlow Lite Micro inference, esp-vision's module compiled as it
# is (modules/.esp-vision). C only; ESP32-P4 and ESP32-S3.
#
# Model.predict() takes and returns ulab ndarrays: name ulab with it, or use
# all (a module never brings another).
c_module(".")
