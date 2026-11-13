# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""The renders svgio's tests compare with the reference PNGs: (name, width).

`name` is a file in svg/, rendered `width` pixels wide with the height that
follows. make_references.py renders the same list with resvg into
references/. Every file here reads in both renderers alike: paths, basic
shapes, solid fills and strokes, linear and radial gradients, opacity and
transforms.
"""

CASES = (
    ("circle", 48),
    ("wifi", 48),
    ("battery", 48),
    ("home", 48),
    ("gear", 48),
    ("gear", 96),
    ("linear", 64),
    ("linear", 128),
    ("radial", 48),
    ("transform", 40),
    ("transform", 80),
    ("opacity", 40),
    ("curves", 24),
    ("curves", 48),
    ("gauge", 200),
    ("gauge", 400),
)

# Cases also rendered with no alpha buffer, blended over this RGB565 grey.
BLENDED = ("wifi", "opacity", "radial")
BACKGROUND = 0x8410
