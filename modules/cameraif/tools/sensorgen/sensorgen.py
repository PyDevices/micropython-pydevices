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
import os
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

# The controls beyond exposure, gain and the flips that the engine can carry,
# by the name sensorgen's trace gives them (the engine's `controls` keys).
ENGINE_CONTROLS = {"ae_level", "brightness", "contrast", "saturation", "sharpness", "denoise",
                   "special_effect", "wb"}

# Registers a driver reads and acts on while setting a mode, and the value the
# chip holds in them before anything is written. Not in a datasheet we can
# quote: found on the board (the registers the driver then wrote tell it), and
# checked there the same way, so a wrong one shows as a register that differs.
PRELOAD = {
    "ov5647": {0x3037: 0x03},
}

# Sensors a description has been run on, and where: their header says so. Every
# other description is "written from esp_cam_sensor data, not yet run on a
# sensor". Set by hand when a board check passes.
CONFIRMED = {
    "ov2710": "an ESP32-P4-EYE, 2026-11-11: every register of its two modes and the flip, mirror and test pattern states read back equal to the driver's, found by auto-detect with no driver in the image, and a 1920x1080 frame looks right; the ae_level table was not set on the board",
    "ov5647": "an ESP32-P4 Devkit, 2026-11-11: every register of its five modes, exposure, flip, mirror and the test pattern read back equal to the driver's, found by auto-detect with no driver in the image",
}

# What the table engine can't express today, by the short name the fit uses.
GAPS = {
    "control_map": "a control the engine's fields and tables can't take",
    "stream_rmw": "stream on/off reads registers the engine can't mask",
    "stream_toggle": "stream on and off write the same thing (a toggle), so the engine has to know the state",
    "flip_extra": "a flip the engine's lists can't take",
    "format_reads": "setting a mode reads registers and writes what it read",
    "value16": "registers take 16-bit values",
    "mono": "a monochrome sensor (Bayer MONO); the engine names only the four colour orders",
    "line_sync": "a mode uses CSI line sync",
    "timer": "the driver runs a timer while streaming",
    "no_bayer": "no Bayer mode: cameraif's ISP takes only RAW, with this driver or a description",
    "trace_failed": "the driver didn't run on the host",
    "gamma_curve": "the tuning's gamma is a curve of 16 points, not a power; cameraif keeps its default exponent",
    "tuning_by_rev": "the tuning differs by chip revision (eco4 / eco5); this is the one for the chip's later revision",
    "id_pre": "the ID check writes first",
    "test_pattern": "the test pattern is left out: the driver's writes depend on a register bank it remembers",
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
        env = dict(os.environ)
        pre = PRELOAD.get(drv.name)
        if pre:
            env["TRACE_PRELOAD"] = ",".join(f"{r:x}={v:x}" for r, v in pre.items())
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300, env=env)
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


def ops_from(z: list, o: list) -> list | bool:
    """A probe's writes as (register, value), (register, value, mask) and (None, ms).

    `z` ran with every read answering 0, `o` with all ones. Where they write
    the same, the write doesn't depend on a read; where they differ, the bits
    that differ came from the register, and the rest are the mask.
    """
    zs = [e for e in z if e[0] in ("W", "D")]
    os_ = [e for e in o if e[0] in ("W", "D")]
    if len(zs) != len(os_):
        return False
    out = []
    for a, b in zip(zs, os_):
        if a[0] != b[0]:
            return False
        if a[0] == "D":
            if a[2] != b[2]:
                return False
            out.append((None, a[2]))
            continue
        if a[1] != b[1]:
            return False
        w0, w1 = a[2], b[2]
        if w0 == w1:
            out.append((a[1], w0))
            continue
        keep = (w0 ^ w1) & 0xFF
        if (w0 & keep) or (w1 & keep) != keep:
            return False
        mask = ~keep & 0xFF
        out.append((a[1], w0 & mask, mask))
    return out


def single_mask(pair: tuple) -> tuple | None:
    """(register, bits, None, inverted) when on and off each set or clear one register's bits."""
    on, off = pair
    if len(on) != 1 or len(off) != 1 or len(on[0]) != 3 or len(off[0]) != 3:
        return None
    (ra, va, ma), (rb, vb, mb) = on[0], off[0]
    if ra != rb or ma != mb:
        return None
    if vb == 0 and va == ma:
        return (ra, ma, None, False)
    if va == 0 and vb == ma:
        return (ra, ma, None, True)
    return None


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
        ev = tr.detect
        first = next((k for k, e in enumerate(ev) if e[0] == "R"), None)
        ids = []
        k = first if first is not None else len(ev)
        while k < len(ev) and ev[k][0] == "R":
            ids.append(ev[k][1])
            k += 1
        pre = [(e[1], e[2]) if e[0] == "W" else (None, e[2]) for e in ev[: first or 0] if e[0] in ("W", "D")]
        if len(ids) == 2 and ids[1] == ids[0] + 1:
            d["id"] = (ids[0], tr.pid)
        elif len(ids) in (1, 2):
            d["id"] = (tuple(ids), tr.pid)
        else:
            self.gap("control_map", "the ID is read from " + (", ".join(f"{r:#06x}" for r in ids) or "nowhere"))
        if pre:
            d["id_pre"] = pre
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
            reads = sorted(reads - set(PRELOAD.get(self.drv.name, {})))
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

    # --- bit controls: the stream, the flips and the test pattern ---------------

    def bit_ops(self, i: int, name: str, arg: int) -> list | None:
        """What a 0/1 control writes for `arg`, as (register, value) and (register, value, mask).

        Each probe ran twice, with every read answering 0 and then all ones;
        a bit the driver keeps from its read shows as the difference.
        """
        z = next((ev for a, ev in self.phase(i, f"{name}_zero") if a == arg), None)
        o = next((ev for a, ev in self.phase(i, f"{name}_ones") if a == arg), None)
        if z is None or o is None:
            return None
        r = ops_from(z, o)
        return r if r is False else tuple(r)

    def fit_stream(self) -> None:
        per = {}
        for i in self.bayer:
            on, off = self.bit_ops(i, "stream", 1), self.bit_ops(i, "stream", 0)
            if on is None or off is None:
                continue
            timers = [e[1] for e in self.phase(i, "stream")[0][1] if e[0] == "T"] if self.phase(i, "stream") else []
            if timers:
                self.gap("timer", "streaming starts " + ", ".join(timers))
            per[i] = (on, off)
        if not per:
            return
        if any(on is False or off is False for on, off in per.values()):
            self.gap("stream_rmw", "the stream's reads aren't a mask")
            return
        toggle = any(on == off and on for on, off in per.values())
        if toggle and not all(on == off for on, off in per.values()):
            self.gap("stream_toggle", "only some modes toggle")
            return
        if toggle:
            self.desc["stream_toggle"] = True
        common = max(set(per.values()), key=lambda p: list(per.values()).count(p))
        self.desc["stream"] = (list(common[0]), list(common[1]))
        # A mode that does it differently carries its own lists.
        for k, i in enumerate(self.bayer):
            if i in per and per[i] != common:
                self.desc["modes"][k]["stream"] = (list(per[i][0]), list(per[i][1]))

    def fit_pair(self, name: str) -> tuple | None:
        """(on, off) op lists for a bit control, equal in every mode, or None with a gap."""
        def get(i: int, arg: int):
            return self.bit_ops(i, name, arg)
        pairs = {(get(i, 1), get(i, 0)) for i in self.bayer}
        if len(pairs) != 1:
            self.gap("control_map", f"{name} differs by mode")
            return None
        on, off = pairs.pop()
        if not on or not off:
            if (on is False or off is False) and name != "test_pattern":
                self.gap("control_map", f"{name} reads registers it keeps part of")
            return None
        return list(on), list(off)

    def fit_flip(self) -> None:
        params = self.tr.blocks[self.bayer[0]]["params"]
        names = [n for n in ("vflip", "hmirror") if n in params]
        if not names:
            return
        pairs = {}
        for n in names:
            p = self.fit_pair(n)
            if p is None:
                self.gap("flip_extra", f"{n} wasn't probed")
                return
            pairs[n] = p
        simple = {n: single_mask(p) for n, p in pairs.items()}
        regs = {s[0] for s in simple.values() if s}
        if all(simple.values()) and len(regs) == 1 and not any(s[3] for s in simple.values()):
            v, hm = simple.get("vflip"), simple.get("hmirror")
            self.desc["flip"] = ((v or hm)[0], v[1] if v else 0, hm[1] if hm else 0)
            return
        self.desc["flip"] = {"v": pairs.get("vflip"), "h": pairs.get("hmirror")}

    def fit_pattern(self) -> None:
        if not any(self.phase(i, "test_pattern_zero") for i in self.bayer):
            return
        p = self.fit_pair("test_pattern")
        if p is None:
            if any(self.bit_ops(i, "test_pattern", a) is False for i in self.bayer for a in (0, 1)):
                self.gap("test_pattern")
            return
        on, off = p
        plain = lambda ops: all(len(o) == 2 and o[0] is not None for o in ops)
        if plain(on) and plain(off):
            regs = [a for a, _ in on]
            if regs == [a for a, _ in off] and regs == list(range(regs[0], regs[0] + len(regs))):
                self.desc["test_pattern"] = (regs[0], len(regs), concat(on), concat(off))
                return
        self.desc["test_pattern"] = (on, off)

    # --- numeric controls ---------------------------------------------------------

    def fit_controls(self) -> None:
        p0 = self.tr.blocks[self.bayer[0]]["params"]
        names = [n for n in sorted(p0) if n not in ("vflip", "hmirror")]
        gain_name = "angain" if "angain" in names else "gain"
        for name in names:
            if name not in USED:
                self.dropped.append(name)
                continue
            if name in ("angain", "gain") and name != gain_name:
                continue
            ctl = self.fit_control(name)
            if ctl is None:
                continue
            if name == "exposure_val":
                self.desc["exposure"] = ctl
            elif name in ("angain", "gain"):
                self.desc["gain"] = ctl
            elif name in ENGINE_CONTROLS:
                self.desc.setdefault("controls", {})[name] = ctl
            else:
                self.dropped.append(name)

    def probes(self, i: int, name: str) -> tuple[list, list]:
        """(value, ops) for a control in one mode: from the zero and ones reads when the
        driver reads a register, else from the plain run. Returns the values that fit
        a table too (all of them) as the second list."""
        plain = [(v, ev) for v, ev in self.phase(i, name)]
        rz = dict(self.phase(i, name + "_rz"))
        ro = dict(self.phase(i, name + "_ro"))
        reads = any(e[0] == "R" for _, ev in plain for e in ev)
        if not reads:
            return [(v, ops_from(ev, ev)) for v, ev in plain], []
        return [(v, ops_from(rz[v], ro[v])) for v in rz if v in ro], []

    def fit_control(self, name: str) -> object | None:
        found = []
        for i in self.bayer:
            p = self.tr.blocks[i]["params"].get(name)
            if p is None:
                return None
            rows, _ = self.probes(i, name)
            if not rows or any(ops is False for _, ops in rows):
                self.gap("control_map", f"{name} reads registers it keeps part of")
                return None
            found.append((i, p, rows))
        # One fit over what every mode wrote: a mode with a short range never moves
        # a high byte, and another that does shows it.
        merged = {}
        for _, _, rows in found:
            for v, ops in rows:
                if merged.setdefault(v, ops) != ops:
                    self.gap("control_map", f"{name} differs by mode")
                    return None
        widest = max((p for _, p, _ in found), key=lambda p: p.get("max", p.get("count", 0)))
        c = self.fit_rows(name, found[0][0], widest, sorted(merged.items()))
        if c is None:
            return None
        ctls = [c]
        return self.finish_limits(name, ctls, found)

    def fit_rows(self, name: str, i: int, p: dict, rows: list) -> dict | None:
        rows = sorted(rows, key=lambda r: r[0])
        # Every probe writes the same registers in the same order.
        layout = {tuple((o[0], None if len(o) == 2 else o[2]) for o in ops) for _, ops in rows}
        if len(layout) != 1:
            self.gap("control_map", f"{name} writes different registers by setting")
            return None
        layout = next(iter(layout))
        if any(r is None for r, _ in layout):
            self.gap("control_map", f"{name} waits between writes")
            return None
        n = len(layout)
        data = [[ops[j][1] for _, ops in rows] for j in range(n)]
        vals = [v for v, _ in rows]
        masks = [m if m is not None else 0xFF for _, m in layout]
        vary = [j for j in range(n) if len(set(data[j])) > 1]
        if not vary:
            self.gap("control_map", f"{name} writes the same whatever the value")
            return None
        lo, hi = vary[0], vary[-1]
        const = [j for j in range(lo, hi + 1) if j not in vary]
        if any(masks[j] != 0xFF for j in const):
            self.gap("control_map", f"{name} keeps part of a register between its values")
            return None
        # Constant writes before the first varying one and after the last are
        # lists of their own (a group hold, a trigger bit); between, constant fields.

        def edge(rng):
            return tuple((layout[j][0], data[j][0]) if masks[j] == 0xFF else (layout[j][0], data[j][0], masks[j])
                         for j in rng)
        before, after = edge(range(0, lo)), edge(range(hi + 1, n))
        shifts = {}
        for j in vary:
            s = next((s for s in range(-8, 24)
                      if all(d == ((v >> s) if s >= 0 else (v << -s)) & 0xFF & masks[j] for v, d in zip(vals, data[j]))), None)
            if s is None:
                break
            shifts[j] = s
        else:
            fields = tuple((layout[j][0], data[j][0], 0) if j in const else (layout[j][0], shifts[j], masks[j])
                           for j in range(lo, hi + 1))
            return {"fields": fields, "table": None, "before": before, "after": after,
                    "p": p, "vals": vals, "mode": i}
        # A table: every setting from min to max has to have been probed.
        first, last = (0, p["count"] - 1) if p["type"] == 3 else (p["min"], p["max"])
        if vals != list(range(first, last + 1)):
            self.gap("control_map", f"{name} isn't linear and only {len(vals)} of its settings were probed")
            return None
        fields = tuple((layout[j][0], data[j][0], 0) if j in const else (layout[j][0], 0, masks[j])
                       for j in range(lo, hi + 1))
        table = bytes(data[j][k] for k in range(len(vals)) for j in vary)
        return {"fields": fields, "table": table, "before": before, "after": after,
                "p": p, "vals": vals, "mode": i}

    def finish_limits(self, name: str, ctls: list, found: list) -> dict:
        c = ctls[0]
        vts = [self.tr.formats[i]["vts"] for i, _, _ in found]
        ps = [p for _, p, _ in found]
        if ps[0]["type"] == 3:
            lo = [0] * len(ps)
            hi = [p["count"] - 1 for p in ps]
        else:
            lo = [p["min"] for p in ps]
            hi = [p["max"] for p in ps]
        df = [p.get("default", hi[k]) for k, p in enumerate(ps)]
        out = {"fields": c["fields"], "table": c["table"], "min": lo[0]}
        if c["before"]:
            out["before"] = c["before"]
        if c["after"]:
            out["after"] = c["after"]
        if len(set(lo)) != 1:
            self.gap("control_map", f"{name}'s minimum differs by mode")
        mul = next((m for m in (1, 2, 4, 8, 16) if len({h - m * v for h, v in zip(hi, vts)}) == 1), None)
        if name == "exposure_val" and mul is not None and hi[0] - mul * vts[0] <= 0:
            # A limit counted back from the frame length.
            out["max"] = hi[0] - mul * vts[0]
            if mul != 1:
                out["vts_mul"] = mul
        elif len(set(hi)) == 1:
            out["max"] = hi[0]
        else:
            self.gap("control_map", f"{name}'s limit isn't constant or a multiple of the frame length less a constant")
            out["max"] = hi[0]
        out["default"] = df[0] if len(set(df)) == 1 else None
        if out["default"] is None:
            del out["default"]
        return out


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
        if aen["gamma"].get("use_gamma_param", True):
            t["gamma"] = gt[len(gt) // 2]["gamma_param"]  # the middle of its steps by brightness
        else:
            gaps.append("gamma_curve")  # a curve of 16 points, not a power: cameraif's default stays
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
    0xFFFF waits `value` ms, so a longer wait takes several entries. A write
    with a mask (register, value, mask) is preceded by an entry of register
    0xFFFE whose value is the mask: a read-modify-write of those bits."""
    out = bytearray()
    for e in regs:
        a, v = e[0], e[1]
        if a is None:
            while v > 0:
                out += bytes((0xFF, 0xFF, min(v, 255)))
                v -= 255
            continue
        assert 0 <= a < 0xFFFE and 0 <= v <= 0xFF, e
        if len(e) == 3 and e[2] != 0xFF:
            out += bytes((0xFF, 0xFE, e[2]))
        out += bytes((a >> 8, a & 0xFF, v))
    return bytes(out)


def pack_fields(fields: tuple) -> bytes:
    """A control's fields: register high, low, shift (a signed byte), mask."""
    out = bytearray()
    for reg, shift, mask in fields:
        out += bytes((reg >> 8, reg & 0xFF, shift & 0xFF, mask))
    return bytes(out)


def blob_py(raw: bytes, indent: int, width: int = 18) -> str:
    """Bytes as literals, `width` bytes to a line, in parentheses when it takes several."""
    lines = ["b\"" + "".join(f"\\x{b:02x}" for b in raw[i:i + width]) + "\"" for i in range(0, len(raw), width)] or ["b\"\""]
    if len(lines) == 1:
        return lines[0]
    pad = " " * (indent + 4)
    return "(\n" + "\n".join(pad + ln for ln in lines) + "\n" + " " * indent + ")"


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
    where = CONFIRMED.get(fit.drv.name)
    L += ["", f"Confirmed on hardware: {where}." if where
          else f"Written from esp_cam_sensor {version} data, not yet run on a sensor."]
    if gaps:
        L += ["", "Differs from Espressif's driver:", ""]
        L += [f"- {GAPS.get(k, k)}" + (f" ({v})" if v else "") + "." for k, v in sorted(gaps.items())]
    else:
        L += ["", "Differs from Espressif's driver in nothing the engine can see: a replay on a desktop", "writes the same registers, mode by mode and control by control."]
    if fit.dropped:
        L += ["", "Controls cameraif doesn't use, left out: " + ", ".join(sorted(fit.dropped)) + "."]
    if "timer" in fit.gaps:
        L += ["", "The driver's timer only gathers statistics for esp_ipa, which cameraif",
              "doesn't run, so there is no timer here."]
    if fit.drv.name in PRELOAD:
        L += ["", "The driver reads " + ", ".join(f"{r:#06x}" for r in PRELOAD[fit.drv.name])
              + " and acts on it; the value the chip holds before any write is fixed here:",
              "  " + ", ".join(f"{r:#06x} = {v:#04x}" for r, v in PRELOAD[fit.drv.name].items()) + "."]
    L += ['"""', ""]
    if "init" in d:
        L += ["_INIT = (", regs_py(d["init"], rb, 4), ")  # fmt: skip", ""]
    L.append("_MODES = (")
    for m in d["modes"]:
        L.append("    {")
        for k in ("name", "width", "height", "fps", "format", "bayer", "lanes", "lane_hz", "pclk", "hts", "vts", "xclk"):
            if k in m:
                L.append(f"        \"{k}\": {py(m[k])},")
        if "stream" in m:
            L.append("        \"stream\": (")
            L += ["            " + blob_py(pack(lst), 12) + "," for lst in m["stream"]]
            L.append("        ),")
        L += ["        \"regs\": (", regs_py(m["regs"], rb, 12), "        ),", "    },"]
    L += [")  # fmt: skip", "", "SENSOR = {"]
    L.append(f"    \"name\": {py(d['name'])},")
    L.append(f"    \"addr\": 0x{d['addr']:02X},")
    L.append(f"    \"reg_bits\": {rb},")
    if "id" in d:
        r, pid = d["id"]
        r = "(" + ", ".join(hx(x, rb) for x in r) + ")" if isinstance(r, tuple) else hx(r, rb)
        L.append(f"    \"id\": ({r}, 0x{pid:04X}),")
    if "id_pre" in d:
        L.append(f"    \"id_pre\": {blob_py(pack(d['id_pre']), 4)},")
    if "init" in d:
        L.append("    \"init\": _INIT,")
    L.append("    \"modes\": _MODES,")
    if "stream" in d:
        L.append("    \"stream\": (")
        L += ["        " + blob_py(pack(lst), 8) + "," for lst in d["stream"]]
        L.append("    ),")
        if d.get("stream_toggle"):
            L.append("    \"stream_toggle\": True,")
    for k in ("exposure", "gain"):
        if k in d:
            L.append(f"    \"{k}\": {ctrl_py(d[k], rb, 4)},")
    if "controls" in d:
        L.append("    \"controls\": {")
        for k, c in d["controls"].items():
            L.append(f"        \"{k}\": {ctrl_py(c, rb, 8)},")
        L.append("    },")
    if "flip" in d:
        f = d["flip"]
        if isinstance(f, dict):
            L.append("    \"flip\": {")
            for k, pair in f.items():
                if pair:
                    L.append(f"        \"{k}\": (")
                    L += ["            " + blob_py(pack(lst), 12) + "," for lst in pair]
                    L.append("        ),")
            L.append("    },")
        else:
            r, v, hm = f
            L.append(f"    \"flip\": ({hx(r, rb)}, 0x{v:02X}, 0x{hm:02X}),")
    if "test_pattern" in d:
        tp = d["test_pattern"]
        if len(tp) == 4:
            r, n, on, off = tp
            L.append(f"    \"test_pattern\": ({hx(r, rb)}, {n}, 0x{on:X}, 0x{off:X}),")
        else:
            L.append("    \"test_pattern\": (")
            L += ["        " + blob_py(pack(lst), 8) + "," for lst in tp]
            L.append("    ),")
    if tune:
        L.append("    # Tuning, from Espressif's JSON for this sensor.")
        L += [f"    {py(k)}: {py(tune[k])}," for k in ("gamma", "color", "ccm") if k in tune]
        for k in ("ccm_table", "awb_table"):
            if k in tune:
                L += [f"    \"{k}\": ("] + [f"        {py(row)}," for row in tune[k]] + ["    ),"]
    L.append("}")
    return "\n".join(L) + "\n"


def ctrl_py(c: object, rb: int, indent: int) -> str:
    """A control: a tuple when it is a plain value over consecutive registers, else a dict."""
    f = c["fields"]
    simple = (not c.get("table") and "vts_mul" not in c and not c.get("before") and not c.get("after")
              and all(m == 0xFF for _, _, m in f)
              and [r for r, _, _ in f] == list(range(f[0][0], f[0][0] + len(f)))
              and [s for _, s, _ in f] == [8 * (len(f) - 1 - i) for i in range(len(f))])
    df = c.get("default")
    if simple:
        return f"({hx(f[0][0], rb)}, {len(f)}, {c['min']}, {c['max']}, {py(df)})"
    pad = " " * (indent + 4)
    rows = [f"{pad}\"fields\": {blob_py(pack_fields(f), indent + 4)},"]
    if c.get("table"):
        rows.append(f"{pad}\"table\": {blob_py(c['table'], indent + 4, 24)},")
    for k in ("before", "after"):
        if c.get(k):
            rows.append(f"{pad}\"{k}\": {blob_py(pack(c[k]), indent + 4)},")
    rows.append(f"{pad}\"min\": {c['min']},")
    rows.append(f"{pad}\"max\": {c['max']},")
    if df is not None:
        rows.append(f"{pad}\"default\": {df},")
    if "vts_mul" in c:
        rows.append(f"{pad}\"vts_mul\": {c['vts_mul']},")
    return "{\n" + "\n".join(rows) + "\n" + " " * indent + "}"


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
            if "stream" not in fit.desc:
                # The engine can't start it, so a description would give no picture.
                print(f"{n}: no description: {GAPS.get('stream_toggle') if 'stream_toggle' in fit.gaps else fit.gaps}")
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
