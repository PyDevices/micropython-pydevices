# SPDX-FileCopyrightText: 2026 Brad Barnett
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: MIT AND Apache-2.0
"""TensorFlow Lite Micro inference on the ESP32-P4 and ESP32-S3.

`Model` loads a `.tflite` file and runs it. Inputs and outputs are ulab
ndarrays, so a firmware needs the `ulab` module too. This is esp-vision's
`tflite` module, the API OpenMV's `ml` module uses, with one addition:
streaming (stateful) models, such as the microWakeWord models ESPHome
publishes, keep their state between calls in resource variables, and each
`Model` has room for 20.

A streaming model's input is already int8, so fill the tensor with a callable
rather than an ndarray, which `Model.predict` would quantize again.

It is not in `--modules all`: it adds TensorFlow Lite Micro to the image, so
name it when you build.
"""

from typing import Callable, Sequence

__revision__: str
"""Which source this firmware was built from, from `git describe`: a release
tag, or the nearest one and the commit. "unknown" when the build had no git."""

Shape = tuple[int, ...]
"""A tensor shape, for example `(1, 96, 96, 1)`."""
TensorWriter = Callable[[bytearray, Shape, int], None]
"""A callable that fills an input tensor: called with the tensor's own memory,
its shape and its dtype code."""
Postprocess = Callable[["Model", Sequence[object], list[object]], object]
"""A function called as `callback(model, inputs, raw_outputs)` that returns the
final result of `Model.predict`."""

class Model:
    """A loaded TensorFlow Lite Micro model.

    The attributes describe the model's tensors and are read-only.
    """

    len: int
    """The model file's length in bytes."""
    ram: int
    """The runtime tensor arena's size in bytes."""
    input_shape: tuple[Shape, ...]
    """The input tensors' shapes."""
    input_scale: tuple[float, ...]
    """The input quantization scales."""
    input_zero_point: tuple[int, ...]
    """The input quantization zero points."""
    input_dtype: tuple[str, ...]
    """The input dtype codes as strings: "b", "B", "h", "H" or "f"."""
    output_shape: tuple[Shape, ...]
    """The output tensors' shapes."""
    output_scale: tuple[float, ...]
    """The output quantization scales."""
    output_zero_point: tuple[int, ...]
    """The output quantization zero points."""
    output_dtype: tuple[str, ...]
    """The output dtype codes as strings: "b", "B", "h", "H" or "f"."""
    postprocess: Postprocess | None
    """The callback `predict` uses when you don't pass one."""

    def __init__(self, path: str, *, postprocess: Postprocess | None = None) -> None:
        """Load a model from the filesystem.

        Args:
            path: The model file, such as "/sdcard/model.tflite" or
                "/flash/model.tflite".
            postprocess: A callback that receives the model, the inputs and
                the raw outputs and returns the result `predict` gives back.
        """
    def predict(
        self,
        inputs: Sequence[object | TensorWriter],
        *,
        callback: Postprocess | None = None,
    ) -> list[object] | object:
        """Run the model once.

        Args:
            inputs: One item per input tensor. Each is a ulab ndarray, which
                is quantized to the tensor's type, or a callable that fills
                the tensor's raw buffer.
            callback: A one-shot post-processing callback that overrides
                `postprocess`.

        Returns:
            One ulab ndarray per output tensor, in the model's own dtype and
            quantized values, or what the post-processing callback returned.
        """
    def deinit(self) -> None:
        """Free the model, its interpreter and its tensor arena."""
