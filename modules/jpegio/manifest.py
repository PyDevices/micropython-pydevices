# jpegio: JPEG decoding into RGB565 (CircuitPython's jpegio API, on ChaN's
# TJpgDec), and LVGL's JPEG decoder when lvgl-micropython is in the build.
# C only; every port. Moved here from displayif (media modules roadmap,
# Phase 1).
c_module(".")
