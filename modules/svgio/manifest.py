# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

# svgio: SVG files rendered to RGB565 and an alpha mask, on NanoSVG. C only;
# every port. On CPython, pydevices-desktop's svgio.py gives the same API over
# resvg.
c_module(".")
