#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Turn esp_cam_sensor's MIPI drivers into cameraif sensor descriptions.

    python3 sensorgen.py --ecs <esp_cam_sensor source> ov2710 sc2336
    python3 sensorgen.py --ecs <esp_cam_sensor source> --check ov2710 sc2336
    python3 sensorgen.py --ecs <esp_cam_sensor source> --classify

Nothing here copies a number out of Espressif's C by hand. Each driver is
compiled on this computer with the stand-ins in host/ and run against a
pretend chip (host/trace.c), which prints every register it writes while it
detects the sensor, sets each mode, streams, flips and turns each control.
This script fits those writes to the description cameraif's table engine
takes (modes, stream lists, exposure and gain as registers, flip bits), and
reads the colour tuning out of the driver's own JSON. What doesn't fit is
named, sensor by sensor, rather than guessed at.

--check regenerates and compares with what's on disk, so a new
esp_cam_sensor that changes a table fails until the descriptions are
regenerated. --classify traces every MIPI driver and prints what each needs.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOST = HERE / "host"
OUT = HERE.parent.parent / "sensors"  # modules/cameraif/sensors

BAYER = ("RGGB", "GRBG", "GBRG", "BGGR", "MONO")
IDENTITY = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]

# The controls cameraif reads or sets. Anything else a driver offers (frame
# rate, exposure in microseconds, the statistics esp_ipa reads, grouped
# exposure and gain) goes unused, so leaving it out loses nothing.
USED = {
    "exposure_val", "angain", "gain", "vflip", "hmirror", "ae_level",
    "ae_control", "agc", "awb", "wb", "brightness", "contrast", "saturation",
    "sharpness", "denoise", "special_effect", "lenc",
}

# What the table engine can't express today, by the short name the fit uses.
GAPS = {
    "exposure_shift": "exposure sits shifted left in its registers; the engine writes it at bit 0",
    "exposure_order": "exposure registers aren't consecutive, most significant first",
    "gain_map": "gain is an index into register values (and total gains), not one number",
    "control_map": "a control is a table of register values per setting",
    "test_pattern_rmw": "test pattern sets bits in a register the modes don't set, so a full write can't keep the rest",
    "stream_rmw": "stream on/off is a read-modify-write",
    "stream_per_mode": "stream on/off differs between modes",
    "stream_toggle": "stream on and off write the same thing (a toggle), so the engine has to know the state",
    "flip_inverted": "a flip bit is set for off and cleared for on",
    "flip_two_regs": "vertical and horizontal flip are in different registers",
    "flip_extra": "a flip writes more than its own bits",
    "format_reads": "setting a mode reads registers and writes what it read",
    "value16": "registers take 16-bit values",
    "mono": "a monochrome sensor (Bayer MONO); the engine names only the four colour orders",
    "line_sync": "a mode uses CSI line sync",
    "timer": "the driver runs a timer while streaming",
    "no_bayer": "no Bayer mode: cameraif's ISP takes only RAW, with this driver or a description",
    "trace_failed": "the driver didn't run on the host",
    "gamma_curve": "the tuning's gamma is a curve of points; the engine takes one exponent",
    "tuning_by_rev": "the tuning differs by chip revision (eco4 / eco5)",
}

LOGIC = {"format_reads", "trace_failed"}
# Gaps that cost an optional feature, not the picture or its exposure.
MINOR = {"test_pattern_rmw", "gamma_curve", "tuning_by_rev", "timer"}

# Drivers whose logic was decided by reading the C, not by a trace.
BY_READING = {
    "pivariety": "talks to the module's own controller: polls a busy flag, reads its modes and controls at run time",
    "arducam_imx500": "talks to the module's own controller: polls a busy flag, 32-bit commands",
    "lt6911": "an HDMI-to-CSI bridge: its mode follows the HDMI source",
    "tc358743": "an HDMI-to-CSI bridge: EDID, HDMI link and its mode follow the source",
}
DROPPED = {"timer"}  # checked by hand: a timer that only reads statistics nobody uses
TUNING_ONLY = {"gamma_curve", "tuning_by_rev"}


# --- finding and running a driver -------------------------------------------

class Driver:
    def __init__(self, name: str, src: Path, detect: str, addr_macro: str) -> None:
        self.name = name
        self.src = src
        self.dir = src.parent
        self.detect = detect
        self.addr_macro = addr_macro


def find_drivers(ecs: Path) -> dict[str, Driver]:
    found = {}
    pat = re.compile(r"ESP_CAM_SENSOR_DETECT_FN\((\w+),\s*ESP_CAM_SENSOR_MIPI_CSI,\s*(\w+)\)")
    for c in sorted((ecs / "sensors").rglob("*.c")):
        for m in pat.finditer(c.read_text(errors="replace")):
            name = m.group(1)[: -len("_detect")]
            found[name] = Driver(name, c, m.group(1), m.group(2))
    return found


def ecs_version(ecs: Path) -> str:
    p = ecs / "idf_component.yml"
    if p.exists():
        m = re.search(r'^version:\s*"?([\w.]+)', p.read_text(), re.M)
        if m:
            return m.group(1)
    m = re.search(r"(\d+\.\d+\.\d+)", str(ecs))
    return m.group(1) if m else "unknown"


def _incs(drv: Driver, ecs: Path, cfg: Path) -> list[str]:
    return ["-include", str(HOST / "prelude.h"), f"-I{HOST}", f"-I{cfg}",
            f"-I{ecs / 'include'}", f"-I{drv.dir / 'include'}", f"-I{drv.dir / 'private_include'}"]


def resolve(drv: Driver, ecs: Path, cfg: Path, macros: list[str]) -> dict[str, int]:
    """The values of macros in the driver's public headers, through the C preprocessor."""
    hdrs = sorted((drv.dir / "include").glob("*.h"))
    src = "".join(f'#include "{h}"\n' for h in hdrs)
    src += "".join(f"@@{i}@@ {m}\n" for i, m in enumerate(macros))
    r = subprocess.run(["gcc", "-E", "-P", *_incs(drv, ecs, cfg), "-x", "c", "-"],
                       input=src, capture_output=True, text=True, timeout=60)
    vals = {}
    for m in re.finditer(r"@@(\d+)@@ (.+)", r.stdout):
        expr = re.sub(r"(?<=[0-9a-fA-F])[uUlL]+\b", "", m.group(2).strip())
        if re.fullmatch(r"[0-9a-fA-FxX()+\-*/<>| ]+", expr):
            vals[macros[int(m.group(1))]] = eval(expr, {"__builtins__": {}})  # digits and operators only
    return vals


def run_trace(drv: Driver, ecs: Path, cfg: Path, work: Path) -> tuple[str | None, str | None, dict]:
    """Compile the driver with the tracer; return what it printed and what went wrong."""
    # The product ID macro: NAME_PID, or the first of NAME_PID1, NAME_PID2...
    hdr = "".join(h.read_text(errors="replace") for h in sorted((drv.dir / "include").glob("*.h")))
    m = re.search(rf"#define\s+({drv.name.upper()}_PID\w*)", hdr)
    pid_macro = m.group(1) if m else drv.name.upper() + "_PID"
    ids = resolve(drv, ecs, cfg, [pid_macro, drv.addr_macro])
    if pid_macro not in ids:
        return None, f"no {pid_macro}", ids
    exe = work / drv.name
    cmd = ["gcc", "-O0", "-w", "-o", str(exe), f"-DDRIVER_DETECT={drv.detect}",
           f"-DDRIVER_PID={ids[pid_macro]:#x}", *_incs(drv, ecs, cfg),
           str(HOST / "trace.c"), str(drv.src), "-lpthread", "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if r.returncode:
        err = [x for x in r.stderr.splitlines() if "error" in x]
        return None, "compile: " + (err[0] if err else r.stderr[-200:]), ids
    try:
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return None, "the trace timed out", ids
    return r.stdout, (f"trace exit {r.returncode}" if r.returncode else None), ids


# --- reading a trace ---------------------------------------------------------

class Trace:
    """host/trace.c's output, split by phase."""

    def __init__(self, text: str) -> None:
        self.name = None
        self.pid = None
        self.detect = []
        self.formats = {}  # index -> the F line
        self.blocks = {}  # format index -> {"set": events, "params": {}, "phases": {}}
        self.errors = []
        cur = []
        fmt = None
        phase = None
        for line in text.splitlines():
            k, _, rest = line.partition(" ")
            if k == "M":
                phase, _, arg = rest.partition(" ")
                if phase == "detect":
                    cur = self.detect
                elif phase == "detected":
                    self.name, pid = arg.rsplit(" ", 1)
                    self.pid = int(pid, 16)
                elif phase == "format":
                    fmt = int(arg)
                    b = self.blocks[fmt] = {"set": [], "params": {}, "phases": {}}
                    cur = b["set"]
                elif fmt is not None:
                    cur = []
                    self.blocks[fmt]["phases"].setdefault(phase, []).append((int(arg) if arg else None, cur))
            elif k == "F":
                f = json.loads(rest)
                self.formats[f["index"]] = f
            elif k == "P":
                p = json.loads(rest)
                self.blocks[fmt]["params"][p["name"]] = p
            elif k == "E":
                self.errors.append(f"{phase} crash" if rest == "crash" else rest)
            elif k in ("W", "R"):
                reg, val, bits, abits = rest.split()
                cur.append((k, int(reg, 16), int(val, 16), int(bits), int(abits)))
            elif k == "D":
                cur.append(("D", None, int(rest), 0, 0))
            elif k == "T":
                cur.append(("T", rest, 0, 0, 0))


class Hex(int):
    """A register or a mask, written in hex in a description."""


def writes(ev: list) -> list[tuple[int, int]]:
    return [(e[1], e[2]) for e in ev if e[0] == "W"]


def seq(ev: list) -> list[tuple]:
    """Writes and delays, as the engine takes them: (register, value) and (None, ms)."""
    return [(e[1], e[2]) if e[0] == "W" else (None, e[2]) for e in ev if e[0] in ("W", "D")]


def concat(ws: list[tuple[int, int]]) -> int:
    v = 0
    for _, b in ws:
        v = v << 8 | b
    return v


def fmt_name(code: int) -> str:
    return {10: "raw8", 11: "raw10", 12: "raw12", 3: "yuv422", 4: "yuv422", 5: "yuv420",
            1: "rgb565", 2: "rgb565", 6: "rgb888", 13: "grey", 14: "jpeg"}.get(code, f"pixformat {code}")


# --- fitting -----------------------------------------------------------------

class Fit:
    """One driver's trace, fitted to the table engine's description."""

    def __init__(self, drv: Driver, tr: Trace | None, ids: dict) -> None:
        self.drv = drv
        self.tr = tr
        self.ids = ids
        self.gaps = {}  # gap -> detail
        self.dropped = []
        self.desc = {}
        self.proposed = {}  # keys today's engine ignores, in the shape a gap asks for
        self.bayer = []

    def gap(self, key: str, detail: str = "") -> None:
        old = self.gaps.get(key)
        if old is None or (detail and detail not in old.split("; ")):
            self.gaps[key] = "; ".join(filter(None, [old, detail]))

    def run(self) -> Fit:
        tr = self.tr
        self.bayer = [i for i, f in sorted(tr.formats.items())
                      if f["port"] == 1 and f["format"] in (10, 11, 12) and i in tr.blocks]
        if not self.bayer:
            self.gap("no_bayer", ", ".join(sorted({fmt_name(f["format"]) for f in tr.formats.values()})))
            return self
        sets = [e for i in self.bayer for e in tr.blocks[i]["set"] if e[0] in ("W", "R")]
        if {e[3] for e in sets} - {8}:
            self.gap("value16", "value widths " + ", ".join(str(b) for b in sorted({e[3] for e in sets})))
        d = self.desc
        d["name"] = tr.name
        d["addr"] = self.ids.get(self.drv.addr_macro)
        d["reg_bits"] = max({e[4] for e in sets} or {16})
        idr = [e for e in tr.detect if e[0] == "R"]
        if len(idr) == 2 and idr[1][1] == idr[0][1] + 1:
            d["id"] = (idr[0][1], tr.pid)
        else:
            self.gap("control_map", "the ID isn't two consecutive registers, high byte first")
        self.fit_modes()
        self.fit_stream()
        self.fit_flip()
        self.fit_pattern()
        self.fit_controls()
        return self

    def phase(self, i: int, name: str) -> list:
        return self.tr.blocks[i]["phases"].get(name, [])

    def fit_modes(self) -> None:
        tr = self.tr
        modes = []
        for i in self.bayer:
            f = tr.formats[i]
            ev = tr.blocks[i]["set"]
            # A read of a register this mode already wrote gets back a value
            # the table holds (a driver working out banding from its own
            # timing, say). Only a read of something else asks the chip.
            written, reads = set(), set()
            for e in ev:
                if e[0] == "W":
                    written.add(e[1])
                elif e[0] == "R" and e[1] not in written:
                    reads.add(e[1])
            reads = sorted(reads)
            if reads:
                # Still harmless if it writes the same whatever the chip answers.
                z = [s for _, s in self.phase(i, "format_zero")]
                o = [s for _, s in self.phase(i, "format_ones")]
                if not z or not o or seq(z[0]) != seq(ev) or seq(o[0]) != seq(ev):
                    self.gap("format_reads", "set_format reads " + ", ".join(f"{r:#06x}" for r in reads)
                             + " and writes differently by what it reads")
            if any(e[0] == "T" for e in ev):
                self.gap("timer", "set_format starts a timer")
            if f["line_sync"]:
                self.gap("line_sync")
            if f["bayer"] == 4:
                self.gap("mono")
            m = {"name": f["name"], "width": f["width"], "height": f["height"], "fps": f["fps"],
                 "format": fmt_name(f["format"]), "bayer": BAYER[f["bayer"]], "lanes": f["lanes"],
                 "lane_hz": f["mipi_clk"], "pclk": f["pclk"], "hts": f["hts"], "vts": f["vts"],
                 "regs": seq(ev)}
            if f["xclk"] != 24_000_000:
                m["xclk"] = f["xclk"]
            modes.append(m)
        # What every mode writes first becomes init, held once.
        pre = modes[0]["regs"]
        for m in modes[1:]:
            n = 0
            while n < min(len(pre), len(m["regs"])) and pre[n] == m["regs"][n]:
                n += 1
            pre = pre[:n]
        if len(modes) > 1 and len(pre) >= 8:
            self.desc["init"] = pre
            for m in modes:
                m["regs"] = m["regs"][len(pre):]
        self.desc["modes"] = modes

    def fit_stream(self) -> None:
        lists = set()
        for i in self.bayer:
            ph = self.phase(i, "stream")
            on = next((ev for a, ev in ph if a == 1), None)
            off = next((ev for a, ev in ph if a == 0), None)
            if on is None or off is None:
                continue
            if any(e[0] == "R" for e in on + off):
                self.gap("stream_rmw")
            timers = [e[1] for e in on if e[0] == "T"]
            if timers:
                self.gap("timer", "streaming starts " + ", ".join(timers))
            lists.add((tuple(seq(on)), tuple(seq(off))))
        if len(lists) > 1:
            self.gap("stream_per_mode")
        if any(on == off and on for on, off in lists):
            self.gap("stream_toggle")
        if lists:
            on, off = sorted(lists, key=repr)[0]
            self.desc["stream"] = (list(on), list(off))

    def rmw(self, i: int, name: str) -> tuple | None:
        """How a 0/1 control writes, from its probes against all-zero and all-one reads.

        Returns ("rmw", register, mask, inverted), ("plain", on writes, off writes),
        ("extra", registers) when it does more, or None when it wasn't probed.
        """
        ph = {}
        for key in ("zero", "ones"):
            for a, ev in self.phase(i, f"{name}_{key}"):
                ph[(key, a)] = ev
        if len(ph) != 4:
            return None
        if not any(e[0] == "R" for ev in ph.values() for e in ev):
            if writes(ph[("zero", 1)]) == writes(ph[("ones", 1)]) and writes(ph[("zero", 0)]) == writes(ph[("ones", 0)]):
                return ("plain", writes(ph[("zero", 1)]), writes(ph[("zero", 0)]))
            return ("extra", sorted({e[1] for ev in ph.values() for e in ev}))
        w = {k: writes(ev) for k, ev in ph.items()}
        regs = {r for ws in w.values() for r, _ in ws}
        if len(regs) != 1 or any(len(ws) != 1 for ws in w.values()):
            return ("extra", sorted(regs))
        reg = regs.pop()
        z0, z1 = w[("zero", 0)][0][1], w[("zero", 1)][0][1]
        o0, o1 = w[("ones", 0)][0][1], w[("ones", 1)][0][1]
        mask = z0 ^ z1
        if z0 == 0 and o1 == 0xFF and o0 == 0xFF ^ mask:
            return ("rmw", reg, mask, False)
        if z1 == 0 and o0 == 0xFF and o1 == 0xFF ^ mask:
            return ("rmw", reg, mask, True)
        return ("extra", [reg])

    def fit_flip(self) -> None:
        params = self.tr.blocks[self.bayer[0]]["params"]
        names = [n for n in ("vflip", "hmirror") if n in params]
        if not names:
            return
        fl = {}
        for i in self.bayer:
            for n in names:
                r = self.rmw(i, n)
                if r is None or r[0] != "rmw":
                    self.gap("flip_extra", f"{n} writes {r[1] if r else 'nothing probed'}")
                    return
                if fl.setdefault(n, r) != r:
                    self.gap("flip_extra", f"{n} differs by mode")
                    return
        if any(r[3] for r in fl.values()):
            self.gap("flip_inverted", ", ".join(n for n, r in fl.items() if r[3]))
            return
        v, hm = fl.get("vflip"), fl.get("hmirror")
        if v and hm and v[1] != hm[1]:
            self.gap("flip_two_regs", f"{v[1]:#06x} and {hm[1]:#06x}")
            return
        self.desc["flip"] = ((v or hm)[1], v[2] if v else 0, hm[2] if hm else 0)

    def fit_pattern(self) -> None:
        r = self.rmw(self.bayer[0], "test_pattern")
        if r is None:
            return
        if r[0] == "plain":
            on, off = r[1], r[2]
            regs = [a for a, _ in on]
            if regs and regs == [a for a, _ in off] and regs == list(range(regs[0], regs[0] + len(regs))):
                self.desc["test_pattern"] = (regs[0], len(regs), concat(on), concat(off))
            else:
                self.gap("control_map", "test_pattern writes several registers")
            return
        if r[0] != "rmw":
            self.gap("control_map", "test_pattern writes several registers")
            return
        _, reg, mask, inv = r
        # The engine writes the whole register: right only when every mode
        # leaves the same value in the bits it doesn't own.
        bases = set()
        for m in self.desc["modes"]:
            last = None
            for a, v in self.desc.get("init", []) + m["regs"]:
                if a == reg:
                    last = v
            bases.add(last)
        if len(bases) == 1 and None not in bases:
            base = bases.pop()
            on, off = (base & ~mask, base | mask) if inv else (base | mask, base & ~mask)
            self.desc["test_pattern"] = (reg, 1, on, off)
        else:
            self.gap("test_pattern_rmw", f"{reg:#06x} mask {mask:#04x}")
            self.proposed["test_pattern_bits"] = (Hex(reg), Hex(mask))

    def fit_controls(self) -> None:
        p0 = self.tr.blocks[self.bayer[0]]["params"]
        for name in sorted(p0):
            if name in ("vflip", "hmirror"):
                continue
            if name not in USED:
                self.dropped.append(name)
                continue
            per_mode = [(i, self.tr.blocks[i]["params"].get(name), self.phase(i, name)) for i in self.bayer]
            if name == "exposure_val":
                self.fit_exposure(per_mode)
            else:
                self.fit_map(name, per_mode)

    def fit_exposure(self, per_mode: list) -> None:
        shift = None
        regs = None
        offs, mins, defs = set(), set(), set()
        for i, p, probes in per_mode:
            offs.add(self.tr.formats[i]["vts"] - p["max"])
            mins.add(p["min"])
            defs.add(p["default"])
            for v, ev in probes:
                ws = writes(ev)
                if any(e[0] == "R" for e in ev):
                    self.gap("control_map", "exposure reads registers")
                    return
                r = [a for a, _ in ws]
                regs = regs or r
                if r != regs:
                    self.gap("control_map", "exposure writes different registers by value")
                    return
                x = concat(ws)
                s = next((s for s in range(16) if x == (v << s) & ((1 << 8 * len(ws)) - 1)), None)
                if s is None or (shift is not None and s != shift):
                    self.gap("control_map", "exposure isn't the value, shifted")
                    return
                shift = s
        if not regs:
            return
        if regs != list(range(regs[0], regs[0] + len(regs))):
            self.gap("exposure_order", " ".join(f"{a:#06x}" for a in regs))
            return
        if len(offs) != 1 or len(mins) != 1:
            self.gap("control_map", "exposure's limits aren't a fixed distance from the frame length")
            return
        default = defs.pop() if len(defs) == 1 else None
        ctrl = (Hex(regs[0]), len(regs), mins.pop(), -offs.pop(), default)
        if shift:
            self.gap("exposure_shift", f"value << {shift} in {regs[0]:#06x}..{regs[-1]:#06x}")
            self.proposed["exposure_shifted"] = ctrl + (shift,)
        else:
            self.desc["exposure"] = ctrl

    def fit_map(self, name: str, per_mode: list) -> None:
        """A control as registers and the values each setting writes, or one linear register."""
        tables = set()
        for i, p, probes in per_mode:
            regs = None
            rows = []
            for v, ev in sorted(probes, key=lambda t: t[0]):
                ws = writes(ev)
                if any(e[0] == "R" for e in ev):
                    self.gap("control_map", f"{name} is a read-modify-write")
                    return
                r = tuple(a for a, _ in ws)
                regs = r if regs is None else regs
                if r != regs:
                    self.gap("control_map", f"{name} writes different registers by setting")
                    return
                rows.append((v, tuple(b for _, b in ws)))
            tables.add((regs, tuple(rows), json.dumps(p, sort_keys=True)))
        if len(tables) != 1:
            self.gap("control_map", f"{name} differs by mode")
            return
        regs, rows, pj = tables.pop()
        if not regs:
            return
        p = json.loads(pj)
        consecutive = list(regs) == list(range(regs[0], regs[0] + len(regs)))
        if name in ("angain", "gain") and p["type"] == 1 and consecutive and all(
                concat(list(zip(regs, vs))) == v for v, vs in rows):
            self.desc["gain"] = (regs[0], len(regs), p["min"], p["max"], p["default"])
            return
        values = bytes(b for _, vs in rows for b in vs)
        lo = rows[0][0]
        hi = rows[-1][0]
        if name in ("angain", "gain"):
            self.gap("gain_map", f"{len(rows)} settings over " + ", ".join(f"{a:#06x}" for a in regs))
            self.proposed["gain_map"] = {"regs": tuple(Hex(a) for a in regs), "values": values,
                                         "total": tuple(p.get("elements", ())), "default": p.get("default", 0)}
            return
        self.gap("control_map", name)
        self.proposed.setdefault("controls", {})[name] = {
            "regs": tuple(Hex(a) for a in regs), "min": lo, "max": hi, "default": p.get("default", lo), "values": values}


# --- tuning ------------------------------------------------------------------

def tuning(drv: Driver, chip_rev: str) -> tuple[dict, Path | None, list[str]]:
    """The colour tuning in the driver's JSON, in cameraif's tuning keys."""
    cfgdir = drv.dir / "cfg"
    files = sorted(cfgdir.glob("*.json")) if cfgdir.is_dir() else []
    if not files:
        return {}, None, []
    gaps = []
    pick = [f for f in files if f.stem.endswith(f"_{chip_rev}")]
    if pick:
        gaps.append("tuning_by_rev")
    else:
        pick = [f for f in files if "default" in f.stem and "mono" not in f.stem] or files
    src = pick[0]
    j = json.loads(src.read_text())
    j = next(v for v in j.values() if isinstance(v, dict))
    aen, acc, ian, awb = (j.get(k, {}) for k in ("aen", "acc", "ian", "awb"))
    t = {}
    gt = aen.get("gamma", {}).get("table", [])
    if gt:
        t["gamma"] = gt[len(gt) // 2]["gamma_param"]  # the middle of its steps by brightness
        if not aen["gamma"].get("use_gamma_param", True):
            gaps.append("gamma_curve")
    color = {}
    if aen.get("contrast"):
        color["contrast"] = aen["contrast"][0]["value"]  # at the lowest gain
    if acc.get("saturation"):
        color["saturation"] = acc["saturation"][0]["value"]
    if color:
        t["color"] = color
    table = acc.get("ccm", {}).get("table", [])
    if len(table) == 1:
        t["ccm"] = tuple(table[0]["matrix"])
    elif table:
        t["ccm_table"] = [(r["color_temp"], tuple(r["matrix"])) for r in table][:16]
    ref = awb.get("ref_points")
    bp = ian.get("color_temp", {}).get("bp")
    if ref:
        t["awb_table"] = [(r["ct"], r["rg"], r["bg"]) for r in ref]
    elif bp and table:
        # The white points carry no temperatures: they pair one to one with
        # the measured matrices, the identities at each end left out.
        cts = [r["color_temp"] for r in table if r["matrix"] != IDENTITY]
        if len(cts) == len(bp):
            t["awb_table"] = [(ct, w["a0"], w["a1"]) for ct, w in zip(cts, bp)]
    return t, src, gaps


# --- writing a description ---------------------------------------------------

def hx(v: int, reg_bits: int) -> str:
    return f"0x{v:0{4 if reg_bits == 16 else 2}X}"


def pack(regs: list) -> bytes:
    """A register list in the engine's packed form: three bytes an entry, the
    register high byte first, then its low byte, then the value. Register
    0xFFFF waits `value` ms, so a longer wait takes several entries."""
    out = bytearray()
    for a, v in regs:
        if a is None:
            while v > 0:
                out += bytes((0xFF, 0xFF, min(v, 255)))
                v -= 255
        else:
            assert 0 <= a < 0xFFFF and 0 <= v <= 0xFF, (a, v)
            out += bytes((a >> 8, a & 0xFF, v))
    return bytes(out)


def regs_py(regs: list, rb: int, indent: int) -> str:
    """The packed list as bytes literals, six entries to a line."""
    raw = pack(regs)
    lines = []
    for i in range(0, len(raw), 18):
        lines.append(" " * indent + "b\"" + "".join(f"\\x{b:02x}" for b in raw[i:i + 18]) + "\"")
    return "\n".join(lines)


def py(v: object) -> str:
    """Python source for a value, with double-quoted strings."""
    if isinstance(v, dict):
        return "{" + ", ".join(f"{py(k)}: {py(x)}" for k, x in v.items()) + "}"
    if isinstance(v, bytes):
        return "b\"" + "".join(f"\\x{b:02x}" for b in v) + "\""
    if isinstance(v, (tuple, list)):
        inner = ", ".join(py(x) for x in v)
        return f"({inner},)" if len(v) == 1 else f"({inner})"
    if isinstance(v, str):
        return json.dumps(v)
    if isinstance(v, Hex):
        return f"0x{v:04X}" if v > 0xFF else f"0x{v:02X}"
    if isinstance(v, float):
        return repr(round(v, 4))
    return repr(v)


def emit(fit: Fit, tune: dict, tune_src: Path | None, version: str, ecs: Path) -> str:
    d = fit.desc
    rb = d["reg_bits"]
    L = [
        "# SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD",
        "#",
        "# SPDX-License-Identifier: Apache-2.0",
        "#",
        f"# Generated from espressif/esp_cam_sensor {version}, {fit.drv.src.relative_to(ecs)}"
        + (f"\n# and {tune_src.relative_to(ecs)}." if tune_src else "."),
        "# Don't edit it: run modules/cameraif/tools/sensorgen/sensorgen.py again.",
        f'"""The {d["name"]} for cameraif, as data from Espressif\'s driver.',
        "",
        "    import cameraif",
        f"    from {fit.drv.name} import SENSOR",
        "",
        "    cam = cameraif.Camera(..., sensor=SENSOR)",
    ]
    gaps = {k: v for k, v in fit.gaps.items() if k not in DROPPED}
    if gaps:
        L += ["", "What cameraif's table engine can't do yet, so this leaves out:", ""]
        L += [f"- {GAPS.get(k, k)}" + (f" ({v})" if v else "") + "." for k, v in sorted(gaps.items())]
    if fit.dropped:
        L += ["", "Controls cameraif doesn't use, left out: " + ", ".join(sorted(fit.dropped)) + "."]
    if "timer" in fit.gaps:
        L += ["", "The driver's timer only gathers statistics for esp_ipa, which cameraif",
              "doesn't run, so there is no timer here."]
    L += ['"""', ""]
    if "init" in d:
        L += ["_INIT = (", regs_py(d["init"], rb, 4), ")  # fmt: skip", ""]
    L.append("_MODES = (")
    for m in d["modes"]:
        L.append("    {")
        for k in ("name", "width", "height", "fps", "format", "bayer", "lanes", "lane_hz", "pclk", "hts", "vts", "xclk"):
            if k in m:
                L.append(f"        \"{k}\": {py(m[k])},")
        L += ["        \"regs\": (", regs_py(m["regs"], rb, 12), "        ),", "    },"]
    L += [")  # fmt: skip", "", "SENSOR = {"]
    L.append(f"    \"name\": {py(d['name'])},")
    L.append(f"    \"addr\": 0x{d['addr']:02X},")
    L.append(f"    \"reg_bits\": {rb},")
    if "id" in d:
        L.append(f"    \"id\": ({hx(d['id'][0], rb)}, 0x{d['id'][1]:04X}),")
    if "init" in d:
        L.append("    \"init\": _INIT,")
    L.append("    \"modes\": _MODES,")
    if "stream" in d:
        on, off = d["stream"]
        L.append("    \"stream\": (")
        for lst in (on, off):
            L.append("        " + py(pack(lst)) + ",")
        L.append("    ),")
    for k in ("exposure", "gain"):
        if k in d:
            r, n, lo, hi, df = d[k]
            L.append(f"    \"{k}\": ({hx(r, rb)}, {n}, {lo}, {hi}, {py(df)}),")
    if "flip" in d:
        r, v, hm = d["flip"]
        L.append(f"    \"flip\": ({hx(r, rb)}, 0x{v:02X}, 0x{hm:02X}),")
    if "test_pattern" in d:
        r, n, on, off = d["test_pattern"]
        L.append(f"    \"test_pattern\": ({hx(r, rb)}, {n}, 0x{on:X}, 0x{off:X}),")
    if fit.proposed:
        L.append("    # The shape the gaps above ask for. Today's engine ignores these keys.")
        L += [f"    {py(k)}: {py(v)}," for k, v in fit.proposed.items()]
    if tune:
        L.append("    # Tuning, from Espressif's JSON for this sensor.")
        L += [f"    {py(k)}: {py(tune[k])}," for k in ("gamma", "color", "ccm") if k in tune]
        for k in ("ccm_table", "awb_table"):
            if k in tune:
                L += [f"    \"{k}\": ("] + [f"        {py(row)}," for row in tune[k]] + ["    ),"]
    L.append("}")
    return "\n".join(L) + "\n"


# --- the commands ------------------------------------------------------------

def analyse(drv: Driver, ecs: Path, cfg: Path, work: Path, chip_rev: str) -> tuple[Fit, tuple, str | None]:
    text, err, ids = run_trace(drv, ecs, cfg, work)
    tr = Trace(text) if text else None
    fit = Fit(drv, tr, ids)
    # A control a driver doesn't support fails harmlessly; only detect or
    # setting a mode failing means the trace can't be trusted.
    fatal = [e for e in (tr.errors if tr else []) if e.split()[0] in ("detect", "format")]
    if err or fatal:
        fit.gap("trace_failed", "; ".join(filter(None, [err] + fatal))[:200])
    if tr is None or tr.name is None:
        fit.gap("trace_failed", "detect found nothing")
        return fit, ({}, None), text
    fit.run()
    t, src, tgaps = tuning(drv, chip_rev)
    for g in tgaps:
        fit.gap(g)
    return fit, (t, src), text


def klass(fit: Fit) -> str:
    """table: today's engine runs it; data: it needs engine features that are
    data; logic: it needs C; no-bayer: cameraif can't take its output at all."""
    g = set(fit.gaps) - DROPPED
    if fit.drv.name in BY_READING or g & LOGIC:
        return "logic"
    if "no_bayer" in g:
        return "no-bayer"
    # A table of values for an optional control (the test pattern, an AE
    # target) is minor; for exposure, gain or the ID it isn't.
    cm = fit.gaps.get("control_map", "")
    if g - MINOR - {"control_map"} or any(w in cm for w in ("exposure", "gain", "ID")):
        return "data"
    return "table"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ecs", type=Path, required=True, help="an esp_cam_sensor source tree")
    ap.add_argument("--config", type=Path, default=HOST / "config", help="a folder holding sdkconfig.h")
    ap.add_argument("--out", type=Path, default=OUT, help="where descriptions go")
    ap.add_argument("--chip-rev", default="p4_eco5", help="which tuning a per-revision pair picks")
    ap.add_argument("--check", action="store_true", help="compare with what's written; exit 1 on drift")
    ap.add_argument("--classify", action="store_true", help="trace every MIPI driver and print a table")
    ap.add_argument("--json", type=Path, help="with --classify: write the results here too")
    ap.add_argument("--keep-traces", type=Path, help="save each raw trace in this folder")
    ap.add_argument("sensors", nargs="*")
    a = ap.parse_args()
    ecs = a.ecs.resolve()
    drivers = find_drivers(ecs)
    version = ecs_version(ecs)
    names = a.sensors or (sorted(drivers) if a.classify else [])
    rows, bad = [], False
    with tempfile.TemporaryDirectory() as tmp:
        for n in names:
            if n not in drivers:
                sys.exit(f"{n}: no MIPI driver of that name in {ecs}")
            fit, (tune, src), text = analyse(drivers[n], ecs, a.config.resolve(), Path(tmp), a.chip_rev)
            if a.keep_traces and text:
                a.keep_traces.mkdir(parents=True, exist_ok=True)
                (a.keep_traces / f"{n}.trace").write_text(text)
            if a.classify:
                d = fit.desc
                if n in BY_READING:
                    fit.gap("by_reading", BY_READING[n])
                rows.append({
                    "sensor": n, "class": klass(fit), "gaps": fit.gaps, "dropped": fit.dropped,
                    "modes": len(d.get("modes", [])),
                    "regs": sum(len(m["regs"]) for m in d.get("modes", [])) + len(d.get("init", [])),
                    "formats": sorted({fmt_name(f["format"]) for f in fit.tr.formats.values()}) if fit.tr else [],
                })
                continue
            if "modes" not in fit.desc:
                print(f"{n}: nothing to describe: {fit.gaps}", file=sys.stderr)
                bad = True
                continue
            out = emit(fit, tune, src, version, ecs)
            dest = a.out / f"{n}.py"
            if a.check:
                if not dest.exists() or dest.read_text() != out:
                    print(f"{n}: {dest} isn't what esp_cam_sensor {version} gives; run sensorgen.py again")
                    bad = True
                else:
                    print(f"{n}: matches esp_cam_sensor {version}")
            else:
                a.out.mkdir(parents=True, exist_ok=True)
                dest.write_text(out)
                print(f"{n}: wrote {dest} ({klass(fit)}; gaps: {', '.join(sorted(fit.gaps)) or 'none'})")
    for r in rows:
        print(f"{r['sensor']:15} {r['class']:8} modes {r['modes']:2} regs {r['regs']:5}  "
              + "; ".join(k + (f": {v}" if v else "") for k, v in sorted(r["gaps"].items())))
    if a.json:
        a.json.write_text(json.dumps(rows, indent=1) + "\n")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
