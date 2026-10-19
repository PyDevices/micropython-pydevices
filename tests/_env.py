# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Put the pydevices trees some examples lean on on sys.path.

``pydevices`` is resolved as a sibling checkout and simply skipped when it is
absent, so the tests that don't need it still run in a clone of this
repository alone.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

_paths = []

_pydevices = ROOT.parent / "pydevices"
if _pydevices.is_dir():
    _paths.extend([_pydevices / "lib", _pydevices / "utils"])

for _p in _paths:
    _s = str(_p)
    if _p.is_dir() and _s not in sys.path:
        sys.path.insert(0, _s)
