# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Put the omnipython trees some examples lean on on sys.path.

``omnipython`` is resolved as the library checkout build_mp.py keeps at
``modules/.omnipython-lib`` (or a sibling ``omnipython`` or ``pydevices`` checkout), and simply
skipped when it is absent, so the tests that don't need it still run in a clone of this
repository alone.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]

_paths = []

_omnipython = next((p for p in (ROOT.parent / ".omnipython-lib", ROOT.parent / "omnipython", ROOT.parent / "pydevices") if (p / "lib").is_dir()), None)
if _omnipython:
    _paths.extend([_omnipython / "lib", _omnipython / "utils"])

for _p in _paths:
    _s = str(_p)
    if _p.is_dir() and _s not in sys.path:
        sys.path.insert(0, _s)
