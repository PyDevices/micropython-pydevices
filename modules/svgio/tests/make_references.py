# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Render the reference PNGs that svgio's output is compared with.

Renders every case in cases.py with resvg, a desktop renderer that reads far
more SVG than svgio does, to an RGBA PNG in references/. Run it again when a
case or an SVG changes, and commit what it writes.

    pip install resvg-py pillow
    python modules/svgio/tests/make_references.py
"""

import io
import pathlib
import sys

import resvg_py
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from cases import CASES  # noqa: E402

out = HERE / "references"
out.mkdir(exist_ok=True)
for name, width in CASES:
    png = resvg_py.svg_to_bytes(svg_path=str(HERE / "svg" / (name + ".svg")), width=width)
    image = Image.open(io.BytesIO(bytes(png))).convert("RGBA")
    path = out / ("%s-%d.png" % (name, width))
    image.save(path, optimize=True)
    print("%s: %dx%d" % (path.name, image.width, image.height))
