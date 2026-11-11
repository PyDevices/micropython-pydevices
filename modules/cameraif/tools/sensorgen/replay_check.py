#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Replay each generated description against Espressif's own driver.

    python3 replay_check.py --ecs <esp_cam_sensor source> [sensor ...]

sensorgen.py fits a driver's trace to a description. This closes the loop the
other way: it runs the description the way cameraif's table engine does (a
Python model of sensor_table.c, kept line for line beside it) and compares
every register it writes with what the driver wrote, for each mode, the
stream, the flips, the test pattern and every setting of exposure, gain and
the other controls, with reads answering 0 and then all ones as in the trace.
A mismatch means the description, or the model of the engine, is wrong.
"""

from __future__ import annotations

import argparse
import importlib.util
import sys
import tempfile
from pathlib import Path

import sensorgen as sg

HERE = Path(__file__).resolve().parent
SENSORS = HERE.parent.parent / "sensors"

DELAY, MASK = 0xFFFF, 0xFFFE


class Engine:
    """sensor_table.c's register logic, with a register file that answers reads as the trace did."""

    def __init__(self, d: dict) -> None:
        self.d = d
        self.read = 0
        self.log = []

    def write(self, reg: int, val: int, mask: int = 0xFF) -> None:
        if mask != 0xFF:
            val = (self.read & ~mask) | (val & mask)
        self.log.append(("W", reg, val))

    def wlist(self, raw: bytes | None) -> None:
        mask = 0xFF
        for k in range(0, len(raw or b""), 3):
            reg, val = raw[k] << 8 | raw[k + 1], raw[k + 2]
            if reg == DELAY:
                self.log.append(("D", None, val))
            elif reg == MASK:
                mask = val
            else:
                self.write(reg, val, mask)
                mask = 0xFF

    def apply(self, c: object, v: int) -> None:
        if isinstance(c, tuple):
            reg, n, _, _, _ = c
            for i in range(n):
                self.write(reg + i, (v >> (8 * (n - 1 - i))) & 0xFF)
            return
        f, table = c["fields"], c.get("table")
        cols = sum(1 for k in range(0, len(f), 4) if f[k + 3])
        col = 0
        self.wlist(c.get("before"))
        for k in range(0, len(f), 4):
            reg, sh, mask = f[k] << 8 | f[k + 1], f[k + 2], f[k + 3]
            if mask == 0:
                self.write(reg, sh)
                continue
            if table is not None:
                b = table[(v - c["min"]) * cols + col]
                col += 1
            else:
                sh = sh - 256 if sh > 127 else sh
                b = (v >> sh if sh >= 0 else v << -sh) & 0xFF
            self.write(reg, b, mask)
        self.wlist(c.get("after"))

    def orient(self, which: str, on: bool) -> None:
        fl = self.d["flip"]
        if isinstance(fl, tuple):
            reg, v, h = fl
            self.write(reg, v if which == "vflip" else h, v if which == "vflip" else h) if on else self.write(reg, 0, v if which == "vflip" else h)
        else:
            pair = fl["v" if which == "vflip" else "h"]
            self.wlist(pair[0 if on else 1])

    def pattern(self, on: bool) -> None:
        tp = self.d["test_pattern"]
        if len(tp) == 4:
            reg, n, a, b = tp
            v = a if on else b
            for i in range(n):
                self.write(reg + i, (v >> (8 * (n - 1 - i))) & 0xFF)
        else:
            self.wlist(tp[0 if on else 1])


def norm(ev: list) -> list:
    """Writes and delays, runs of delays summed."""
    out = []
    for e in ev:
        if e[0] == "W":
            out.append(("W", e[1], e[2]))
        elif e[0] == "D":
            if out and out[-1][0] == "D":
                out[-1] = ("D", None, out[-1][2] + e[2])
            else:
                out.append(("D", None, e[2]))
    return out


def load(name: str):
    spec = importlib.util.spec_from_file_location(name, SENSORS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.SENSOR


def check(name: str, ecs: Path, cfg: Path, work: Path) -> list[str]:
    drv = sg.find_drivers(ecs)[name]
    text, err, ids = sg.run_trace(drv, ecs, cfg, work)
    tr = sg.Trace(text)
    d = load(name)
    bad = []
    bayer = [i for i, f in sorted(tr.formats.items()) if f["port"] == 1 and f["format"] in (10, 11, 12) and i in tr.blocks]
    for k, i in enumerate(bayer):
        mode = d["modes"][k]
        e = Engine(d)
        e.wlist(d.get("init"))
        e.wlist(mode["regs"])
        if norm(e.log) != norm([x for x in tr.blocks[i]["set"] if x[0] in ("W", "D")]):
            bad.append(f"mode {k} registers differ")
        ph = tr.blocks[i]["phases"]
        # stream, flips, test pattern: both read answers
        for what in ("stream", "vflip", "hmirror", "test_pattern"):
            for rd, tag in ((0, "zero"), (0xFF, "ones")):
                for arg, ev in ph.get(f"{what}_{tag}", []):
                    e = Engine(d)
                    e.read = rd
                    if what == "stream":
                        pair = mode.get("stream", d.get("stream"))
                        e.wlist(pair[0 if arg else 1] if pair else None)
                    elif what == "test_pattern":
                        if "test_pattern" not in d:
                            continue
                        e.pattern(bool(arg))
                    else:
                        if "flip" not in d:
                            continue
                        e.orient(what, bool(arg))
                    if norm(e.log) != norm([x for x in ev if x[0] in ("W", "D")]):
                        bad.append(f"mode {k} {what} {arg} (reads {tag}) differ")
        # numeric controls
        for cname, key in (("exposure_val", "exposure"), ("angain", "gain"), ("gain", "gain")):
            ctl = d.get(key)
            if ctl is None or cname not in tr.blocks[i]["params"] or (cname == "gain" and "angain" in tr.blocks[i]["params"]):
                continue
            n_bad = 0
            for tag, rd in (("rz", 0), ("ro", 0xFF), ("", 0)):
                for v, ev in ph.get(f"{cname}_{tag}" if tag else cname, []):
                    if not tag and ph.get(f"{cname}_rz"):
                        continue          # the reads answered from memory; rz and ro are the exact ones
                    e = Engine(d)
                    e.read = rd
                    e.apply(ctl, v)
                    if norm(e.log) != norm([x for x in ev if x[0] in ("W", "D")]):
                        n_bad += 1
            if n_bad:
                bad.append(f"mode {k} {cname}: {n_bad} settings differ")
        for cname, ctl in d.get("controls", {}).items():
            n_bad = 0
            for tag, rd in (("rz", 0), ("ro", 0xFF), ("", 0)):
                for v, ev in ph.get(f"{cname}_{tag}" if tag else cname, []):
                    if not tag and ph.get(f"{cname}_rz"):
                        continue
                    e = Engine(d)
                    e.read = rd
                    e.apply(ctl, v)
                    if norm(e.log) != norm([x for x in ev if x[0] in ("W", "D")]):
                        n_bad += 1
            if n_bad:
                bad.append(f"mode {k} {cname}: {n_bad} settings differ")
    return bad


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ecs", type=Path, required=True)
    ap.add_argument("sensors", nargs="*", help="default: every description in sensors/")
    a = ap.parse_args()
    names = a.sensors or sorted(p.stem for p in SENSORS.glob("*.py"))
    failed = False
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            bad = check(n, a.ecs.resolve(), sg.HOST / "config", Path(tmp))
            print(f"{n}: " + ("replays the driver" if not bad else "; ".join(bad)))
            failed |= bool(bad)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
