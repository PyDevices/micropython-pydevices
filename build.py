#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT

"""Build OmniPython firmware: the name to run.

    ./build.py --port esp32 --board ESP32_GENERIC_P4 --modules audiodsp,displayif

This is the command to use for MicroPython and CircuitPython firmware. For now
the code lives in build_mp.py, which this runs with the same arguments, so
`./build.py --help` shows every option.
"""

import runpy
from pathlib import Path

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().with_name("build_mp.py")), run_name="__main__")
