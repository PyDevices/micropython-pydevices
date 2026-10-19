"""Judge espcamera_controls.py's output against the OV5647 driver's own source.

The expectation comes from esp_cam_sensor's ov5647.c (which parameter ids its
query_para_desc answers, and whether its ioctl has a test pattern), mapped to
espcamera's names by the table below, written here and not taken from
cameraif. Behaviour is judged from frames: a vflip must look like the upright
frame turned over, not just differ from it.

    python judge_espcamera_controls.py OUTPUT.json path/to/ov5647.c
"""
import json
import re
import sys

ESPCAMERA_BY_ID = {
    "VFLIP": ("vflip", "flip"), "HMIRROR": ("hmirror", "mirror"),
    "EXPOSURE_VAL": ("aec_value", "exposure"), "GAIN": ("agc_gain", "gain"),
    "ANGAIN": ("agc_gain", "gain"), "BRIGHTNESS": ("brightness", "brightness"),
    "CONTRAST": ("contrast", "contrast"), "SATURATION": ("saturation", "saturation"),
    "SHARPNESS": ("sharpness", "sharpness"), "DENOISE": ("denoise", "denoise"),
    "AWB": ("whitebal", "awb"), "AGC": ("gain_ctrl", "agc"),
    "AE_CONTROL": ("exposure_ctrl", "exposure_ctrl"), "AE_LEVEL": ("ae_level", "ae_level"),
    "WB": ("wb_mode", "wb_mode"), "SPECIAL_EFFECT": ("special_effect", "special_effect"),
    "LENC": ("lenc", "lenc"),
}


def driver_offers(src):
    body = src[src.index("query_para_desc"):]
    body = body[:body.index("\n}\n")]
    ids = set(re.findall(r"case ESP_CAM_SENSOR_([A-Z_]+):", body))
    ioctl = src[src.index("_priv_ioctl"):]
    pattern = "ESP_CAM_SENSOR_IOC_S_TEST_PATTERN" in ioctl[:ioctl.index("\n}\n")]
    return ids, pattern


def main():
    out = json.load(open(sys.argv[1]))
    ids, pattern = driver_offers(open(sys.argv[2]).read())
    want = {ESPCAMERA_BY_ID[i][0] for i in ids if i in ESPCAMERA_BY_ID}
    want_ctl = {ESPCAMERA_BY_ID[i][1] for i in ids if i in ESPCAMERA_BY_ID}
    if pattern:
        want.add("colorbar")
    want |= {"quality", "whitebal"}     # jpegio's, and the ISP's white balance
    fails = []
    rows = []
    for n, r in out["names"].items():
        reach = r[0] == "reads"
        ok = reach == (n in want)
        if not reach and ("controls()" not in r[1] or r[-1]):
            ok = False                   # must say where to look, and hasattr must be False
        if reach and not r[-1]:
            ok = False
        rows.append((n, "reachable" if reach else "raises", r[1], "ok" if ok else "FAIL"))
        if not ok:
            fails.append(n)
    got_ctl = set(out["controls"])
    if got_ctl != want_ctl:
        fails.append("controls() is %s, the driver offers %s" % (sorted(got_ctl), sorted(want_ctl)))
    b = out["behaviour"]
    ux, uy, _ = b["upright"]
    wander = max(abs(b["upright_again"][0] - ux), abs(b["upright_again"][1] - uy))
    tol = wander + 4
    # A sensor flip reverses the readout around the mode's window, which needn't
    # be centred in the array, so the peak lands reflected but not about the
    # frame's middle. What can't change: a vflip keeps x and moves y, a hmirror
    # keeps y and moves x, and each comes back when unset.
    for n, keep, move in (("vflip", 0, 1), ("hmirror", 1, 0)):
        r = b[n]
        good = "error" not in r
        if good:
            p, back = r["peak"], r["after_reset"]
            good = (abs(p[keep] - b["upright"][keep]) <= tol and abs(p[move] - b["upright"][move]) > 3 * tol
                    and max(abs(back[0] - ux), abs(back[1] - uy)) <= tol)
        rows.append((n + " (frames)", "behaves" if good else "wrong", json.dumps(r), "ok" if good else "FAIL"))
        if not good:
            fails.append(n + " does not do what its name says")
    r = b["aec_value"]
    # ov5647_set_AE_target(): 0x3a0f = v * 27 / 25, 0x3a10 = v * 23 / 25
    good = "error" not in r and all(
        r["regs"][str(v)][:2] == [v * 27 // 25, v * 23 // 25] for v in (10, 235))
    rows.append(("aec_value (sensor)", "behaves" if good else "wrong", json.dumps(r), "ok" if good else "FAIL"))
    if not good:
        fails.append("aec_value does not reach the sensor's exposure target")
    r = b["colorbar"]
    good = "error" not in r and abs(r["pattern_mean"] - r["scene_mean"]) > 30
    rows.append(("colorbar (frames)", "behaves" if good else "wrong", json.dumps(r), "ok" if good else "FAIL"))
    if not good:
        fails.append("colorbar does not show the test pattern")
    print("upright peak:", b["upright"], "wander between two upright frames:", wander)
    print("driver (ov5647.c) offers:", sorted(ids), "test pattern" if pattern else "")
    for row in rows:
        print("  %-20s %-10s %-8s %s" % (row[0], row[1], row[3], row[2]))
    print("controls():", out["controls"])
    if fails:
        print("FAIL:", "; ".join(fails))
        sys.exit(1)
    print("PASS")


main()
