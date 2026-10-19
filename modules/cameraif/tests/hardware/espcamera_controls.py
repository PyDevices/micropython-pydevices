"""Every espcamera control name on a real camera, checked against its driver.

Reads each of espcamera's control names, and for the ones the sensor offers,
sets them and checks the sensor did what the name says: a vflip moves the
brightest point in y only, a hmirror in x only, aec_value lands in the
sensor's exposure-target registers, colorbar shows the test pattern. Written
for an OV5647 on SCCB pins 7 and 8 of I2C port 1 (the Waveshare ESP32-P4
WIFI6 DEV-KIT); change the constructor for another board.

    mpremote run espcamera_controls.py | grep '^@@' | cut -c4- > out.json
    python judge_espcamera_controls.py out.json path/to/esp_cam_sensor/sensors/ov5647/ov5647.c

The judge takes what the sensor should offer from its driver's source, not
from cameraif, so a control mapped onto the wrong parameter fails here even
though controls() and the property would still agree.
"""
import json
import espcamera

NAMES = ("contrast", "brightness", "saturation", "sharpness", "denoise", "gain_ceiling",
         "quality", "colorbar", "whitebal", "gain_ctrl", "exposure_ctrl", "hmirror", "vflip",
         "aec2", "awb_gain", "agc_gain", "aec_value", "special_effect", "wb_mode", "ae_level",
         "dcw", "bpc", "wpc", "raw_gma", "lenc")

cam = espcamera.Camera(7, 8, i2c=1, format="MIPI_2lane_24Minput_RAW8_800x800_50fps",
                       pixel_format=espcamera.PixelFormat.GRAYSCALE)
W, H = cam.width, cam.height
out = {"controls": cam.controls(), "size": [W, H], "names": {}}

for n in NAMES:
    try:
        v = getattr(cam, n)
        out["names"][n] = ["reads", repr(v)]
    except AttributeError as e:
        out["names"][n] = ["raises", str(e)]
    # hasattr must agree, since that is how espcamera code asks
    out["names"][n].append(hasattr(cam, n))


def grab(settle=4):
    for _ in range(settle):
        f = cam.take(1)
    return bytes(f)


def peak(b):
    # Where the brightest point is, and how bright: a lamp or an LED in a
    # dark room, the brightest patch in a lit one.
    m = max(b)
    i = b.find(bytes([m]))
    return [i % W, i // W, m]


def mean(a):
    return sum(a[i] for i in range(0, len(a), 97)) * 97 // len(a)


beh = {}
for _ in range(40):
    cam.take(1)
up = peak(grab(6))
beh["upright"] = up
beh["upright_again"] = peak(grab(2))      # the floor: how far the peak wanders on its own
for name in ("vflip", "hmirror"):
    try:
        setattr(cam, name, True)
        p = peak(grab(8))
        setattr(cam, name, False)
        beh[name] = {"peak": p, "after_reset": peak(grab(8))}
    except AttributeError as e:
        beh[name] = {"error": str(e)}
try:
    # In a dark room the picture can't show an exposure target (the sensor is
    # at full exposure for any of them), so ask the sensor: the OV5647 driver
    # writes the target's window into 0x3a0f (high) and 0x3a10 (low).
    regs = {}
    for v in (10, 235):
        cam.aec_value = v
        _ = grab(2)
        regs[v] = [cam.reg(0x3A0F), cam.reg(0x3A10), cam.aec_value]
    beh["aec_value"] = {"regs": regs}
except AttributeError as e:
    beh["aec_value"] = {"error": str(e)}
try:
    a = grab(2)
    cam.colorbar = True
    p = grab(6)
    cam.colorbar = False
    beh["colorbar"] = {"scene_mean": mean(a), "pattern_mean": mean(p)}
except AttributeError as e:
    beh["colorbar"] = {"error": str(e)}
try:
    cam.quality = 30
    beh["quality"] = {"set": cam.quality}
except Exception as e:
    beh["quality"] = {"error": repr(e)}
out["behaviour"] = beh
cam.deinit()
print("@@", json.dumps(out))
