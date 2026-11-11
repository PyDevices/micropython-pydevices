# SPDX-FileCopyrightText: 2026 Brad Barnett
#
# SPDX-License-Identifier: MIT
"""Read back what a camera's registers hold under a driver and under its description.

Run it on an ESP32-P4 board once per arm, then compare the files on a desktop
with `board_compare.py`:

    driver       Espressif's C driver (no sensor=)
    description  the description from /<name>.py, passed as sensor=
    frozen       whatever Camera() finds with no sensor=, which on an image
                 without the driver is a frozen description

    mpftp put ../../sensors/ov2710.py /ov2710.py
    mpftp put board_check.py /board_check.py
    mpftp exec "import board_check; board_check.run('ov2710', 'driver')"

For each mode it opens the camera with auto-exposure off, reads every
register the description's lists write, then sets exposure, gain, flip, mirror
and the test pattern to a few values and reads the registers each touches.
Results go to /bc_<name>_<arm>.json, and a JPEG of the first mode beside it.
"""

import json
import time


def _list_regs(raw):
    """Registers a packed list writes."""
    out = []
    for i in range(0, len(raw), 3):
        reg = raw[i] << 8 | raw[i + 1]
        if reg == 0xFFFF:
            continue
        if reg == 0xFFFE:
            continue
        out.append(reg)
    return out


def _ctrl_regs(c):
    if c is None:
        return []
    if isinstance(c, tuple):
        return [c[0] + i for i in range(c[1])]
    f = c["fields"]
    return [f[i] << 8 | f[i + 1] for i in range(0, len(f), 4)]


def _regs_of(sensor):
    """(mode regs by mode, control registers by control) the description names."""
    modes = []
    for m in sensor["modes"]:
        r = []
        for raw in (sensor.get("init", b""), m["regs"]):
            r += _list_regs(raw)
        modes.append(sorted(set(r)))
    ctl = {
        "exposure": _ctrl_regs(sensor.get("exposure")),
        "gain": _ctrl_regs(sensor.get("gain")),
    }
    fl = sensor.get("flip")
    if isinstance(fl, tuple):
        ctl["flip"] = ctl["mirror"] = [fl[0]]
    elif fl:
        ctl["flip"] = _list_regs(b"".join(fl["v"])) if "v" in fl else []
        ctl["mirror"] = _list_regs(b"".join(fl["h"])) if "h" in fl else []
    tp = sensor.get("test_pattern")
    if tp:
        ctl["test_pattern"] = [tp[0] + i for i in range(tp[1])] if len(tp) == 4 else _list_regs(b"".join(tp))
    return modes, ctl


def _open(sensor, arm, fmt):
    kwargs = {"format": fmt}
    if arm == "description":
        kwargs["sensor"] = sensor
    from board_peripherals import camera

    return camera(**kwargs)


def _read(cam, regs):
    return {"0x%04X" % r: cam.reg(r) for r in regs}


def run(name, arm):
    """Record one arm for the sensor in /<name>.py."""
    sensor = __import__(name).SENSOR
    mode_regs, ctl_regs = _regs_of(sensor)
    out = {"name": name, "arm": arm, "modes": []}
    for k in range(len(sensor["modes"])):
        cam = _open(sensor, arm, k)
        try:
            cam.auto_exposure(False)
            time.sleep_ms(300)
            rec = {"index": k, "sensor": cam.sensor(), "size": cam.size(), "controls": cam.controls()}
            rec["regs"] = _read(cam, mode_regs[k])
            states = {}
            c = cam.controls()
            for key, fn in (("exposure", cam.exposure), ("gain", cam.gain)):
                if key in c and ctl_regs[key]:
                    lo, hi = c[key][0], c[key][1]
                    for v in sorted({lo, lo + 1, (lo + hi) // 2, hi - 1, hi}):
                        fn(v)
                        states["%s=%d" % (key, v)] = _read(cam, ctl_regs[key])
            for key, fn in (("flip", cam.flip), ("mirror", cam.mirror)):
                if ctl_regs.get(key):
                    for on in (True, False):
                        fn(on)
                        states["%s=%d" % (key, on)] = _read(cam, ctl_regs[key])
            if ctl_regs.get("test_pattern"):
                for on in (True, False):
                    try:
                        cam.test_pattern(on)
                    except OSError:
                        pass
                    states["test_pattern=%d" % on] = _read(cam, ctl_regs["test_pattern"])
            rec["states"] = states
            if k == 0:
                cam.auto_exposure(True)
                time.sleep_ms(2500)
                with open("/bc_%s_%s.jpg" % (name, arm), "wb") as f:
                    f.write(cam.capture_jpeg(90))
            out["modes"].append(rec)
        finally:
            cam.deinit()
    with open("/bc_%s_%s.json" % (name, arm), "w") as f:
        json.dump(out, f)
    print(name, arm, "recorded", len(out["modes"]), "modes")
