# tflite — TensorFlow Lite Micro inference

`import tflite` runs a `.tflite` model on an ESP32-P4 or ESP32-S3: load it
with `tflite.Model`, hand `predict()` the input, and read the output as ulab
arrays. It is esp-vision's module, the API OpenMV's `ml` module uses, and it
also runs streaming (stateful) models such as the microWakeWord models ESPHome
publishes.

It is opt-in: `--modules all` leaves it out because it adds TensorFlow Lite
Micro to the image. Name it, and ulab, which `predict()` takes and returns:

```bash
./build_mp.py --port esp32 --board ESP32_GENERIC_P4 --variant C6_WIFI --flash 16MB --modules ulab,tflite
```

```python
import tflite

model = tflite.Model("sine.tflite")
in_scale, in_zero = model.input_scale[0], model.input_zero_point[0]
out_scale, out_zero = model.output_scale[0], model.output_zero_point[0]
print("Loaded %s: %d bytes, %d bytes of working memory" % ("sine.tflite", model.len, model.ram))


def predict(x):
    def fill(buf, shape, dtype):
        buf[0] = max(-128, min(127, round(x / in_scale) + in_zero)) & 0xFF

    output = model.predict([fill])[0]
    return (output[0][0] - out_zero) * out_scale
```

[`examples/tflite_simpletest.py`](examples/tflite_simpletest.py) is that
program whole: it asks a model that learned `sin(x)` for eight values and
prints each beside `math.sin`. Its docstring says where to get the model.

[`tflite.pyi`](tflite.pyi) lists every name, with its arguments.
[Use esp-vision's modules](../../docs/guides/use-esp-vision-modules.md) covers
streaming models and lists the models that have run, each with an example and
its arena size and time.
