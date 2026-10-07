# pngio: PNG encoding (real deflate) and decoding, RGB565 in and out. C only;
# every port with the deflate module (decode uses its uzlib). On CPython,
# pydevices-desktop's pngio.py gives the same API over Pillow.
c_module(".")
