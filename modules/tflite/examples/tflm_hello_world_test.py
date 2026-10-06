# TensorFlow Lite Micro's own hello-world test (hello_world_test.cc,
# LoadQuantModelAndPerformInference), run through the tflite module: the same
# int8 model, the same four raw inputs, the same 0.05 tolerance against sin.
#
#   mpftp put .../tflite-micro/tensorflow/lite/micro/examples/hello_world/models/hello_world_int8.tflite /hello_world_int8.tflite
#   mpftp run tflm_hello_world_test.py
import math

import tflite

model = tflite.Model("/hello_world_int8.tflite")
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]

golden_float = (0.77, 1.57, 2.3, 3.14)
golden_int8 = (-96, -63, -34, 0)
epsilon = 0.05

ok = True
for xf, xq in zip(golden_float, golden_int8):

    def fill(buf, shape, dtype, q=xq):
        buf[0] = q & 0xFF

    out = model.predict([fill])[0]
    try:
        raw = out.reshape((1,))[0]
    except (AttributeError, ValueError):
        raw = out[0]
    y = (raw - out_zero) * out_scale
    err = abs(math.sin(xf) - y)
    ok = ok and err <= epsilon
    print("x=%.2f int8 %4d -> y=%.4f sin=%.4f err=%.4f" % (xf, xq, y, math.sin(xf), err))
print("PASS" if ok else "FAIL", "(TFLM's tolerance %.2f)" % epsilon)
model.deinit()
