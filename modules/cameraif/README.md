# cameraif

A MIPI-CSI camera as a MicroPython object, on the ESP32-P4.

New here? Read the [newcomer's guide](docs/newcomers.md) for the sensor-to-
framebuffer path, firmware boundary, and capture ownership rules.

```python
import cameraif

cam = cameraif.Camera(sda=7, scl=8, reset=21)
print(cam.sensor())                 # ('OV5647', 22087)
print(cam.size())                   # (800, 800, 1280000)

open("still.jpg", "wb").write(cam.capture_jpeg(85))
cam.deinit()
```

The sensor is found by probing, not by being told. `cameraif` asks the
[`esp_cam_sensor`](https://components.espressif.com/components/espressif/esp_cam_sensor)
component which of its drivers answers on the I2C bus you named, then reads
resolution, frame rate, MIPI lane count and lane bit rate out of whatever
driver replied — which is why the class is `Camera` and not `OV5647`.

A camera Espressif doesn't ship can be described in Python instead: its
address, chip ID, register lists, modes and controls, and its colour tuning,
as a dict passed as `sensor=`. No firmware build is needed for a new one; see
[A camera described in Python](#a-camera-described-in-python).

Proven on an ESP32-P4 with an OV5647 at 800x800 and an IMX219 (Raspberry Pi
Camera v2) at up to 1920x1080.

Coming from CircuitPython? On an ESP32-P4, `cameraif` is also `espcamera`,
with its `take()`, `pixel_format`, `frame_size`, `reconfigure()` and its
controls as properties. Your code runs with only the constructor changed:
[Written for espcamera](#written-for-espcamera).

## Why a C module

Three things in the path are not reachable from Python, and each is the
difference between a working camera and a plausible-looking failure:

- **The MIPI D-PHY needs 2.5 V from LDO channel 3** before the sensor's
  clock lane means anything. Without it the sensor answers on I2C perfectly
  and delivers nothing.
- **Frames arrive by DMA into PSRAM**, so the CPU's cached view of that
  memory is stale until it is invalidated. Skip the invalidate and you read
  the buffer's previous contents — for a freshly allocated buffer, a
  flawless frame of black.
- **`esp_cam_ctlr_receive()` does not wait.** It queues a buffer and returns
  as soon as the queue accepts it. Treat that acknowledgement as a frame and
  the camera appears to work at hundreds of frames a second while the buffer
  never changes.

## Install

`cameraif` is a user C module for the ESP32-P4; every line of it is MIPI-CSI
hardware. On rp2 it still builds, with its bodies stubbed and `available()`
answering `False`; MicroPython's Make ports such as unix have no build glue
for it at all. It also builds into CircuitPython-compatible firmware for an
ESP32-P4 (see [On CircuitPython](#on-circuitpython) below).

The esp32 port needs two small patches first: the sensor-driver component and
its sdkconfig (see [patches/](patches/); both apply cleanly to MicroPython
v1.29.0). Point the script at your own MicroPython tree:

```bash
/path/to/cameraif/apply_patches.sh --apply /path/to/micropython
```

micropython-pydevices' `build_mp.py` applies them on its own, with the rest of
its patch series.

Then, on MicroPython 1.29 or later, add one line to the manifest your P4 build
already uses (usually `ports/esp32/boards/manifest.py`, unless your board
brings its own):

```python
include("/path/to/cameraif/manifest.py")
```

The manifest freezes nothing; it only names the C module. If you would rather
pass a manifest of your own as `FROZEN_MANIFEST=`, it replaces the port's
default, so give it `include("$(PORT_DIR)/boards/manifest.py")` as well or you
lose `asyncio` and the port's other frozen modules.

On MicroPython older than 1.29, manifests have no `c_module()`; use the
`USER_C_MODULES` flag instead:

```bash
make USER_C_MODULES=/path/to/cameraif/micropython.cmake BOARD=ESP32_GENERIC_P4
```

Either way, enable a sensor driver in the board's sdkconfig — for the OV5647:

```
CONFIG_CAMERA_OV5647=y
CONFIG_CAMERA_OV5647_AUTO_DETECT_MIPI_INTERFACE_SENSOR=y
```

The `AUTO_DETECT` symbol is what registers the driver's probe in the linker
section the component scans. Set only the first and the driver compiles,
links, and is never asked anything.

### On CircuitPython

CircuitPython's espressif port builds with make, through
[micropython.mk](micropython.mk). It needs the port to build the camera's
ESP-IDF components and the sensor drivers, which it doesn't on its own, so
build with [micropython-pydevices](https://github.com/PyDevices/micropython-pydevices):
its CircuitPython patches add that, and its `build_mp.py` fetches the sensor
drivers at a pinned version.

```bash
./build_mp.py --interpreter circuitpython --port espressif \
    --board waveshare_esp32p4_wifi6_dev_kit --modules cameraif,jpegio,ppa
```

`jpegio` gives `capture_jpeg()` the P4's JPEG engine and `ppa` gives
`capture_scaled()` its scaler. The board's sdkconfig enables the sensor
drivers, as above. On CircuitPython, `i2c=` can also be a `busio.I2C`, such as
`board.I2C()`, and the camera joins that bus. The camera closes at the end of
every program, before CircuitPython releases the board's I2C bus.

## API

### `Camera(sda, scl, **kwargs)`

`sda` and `scl` are required and positional; everything else is keyword-only.
Constructing starts the camera streaming. Only one `Camera` can exist at a
time — the P4 has one CSI controller — and constructing a second one raises.

| Argument | Default | Meaning |
|---|---|---|
| `sda`, `scl` | — | I2C pins the sensor answers on (SCCB) |
| `i2c` | `0` | I2C port. **Use the port that already owns these pins** — see below. On CircuitPython, a `busio.I2C` too |
| `ldo_chan` | `3` | LDO channel powering the MIPI D-PHY |
| `ldo_mv` | `2500` | LDO voltage, millivolts |
| `reset` | `-1` | Sensor reset pin, `-1` for none |
| `pwdn` | `-1` | Sensor power-down pin, `-1` for none |
| `xclk` | `-1` | Pin to generate the sensor's clock on, `-1` if the board has an oscillator |
| `xclk_hz` | `24000000` | Frequency for that clock |
| `format` | `None` | Sensor format by name or index; `None` picks the driver's default |
| `pixel_format` | `None` | What `take()` returns: a `PixelFormat` (`RGB565`, the default, `GRAYSCALE` or `JPEG`). Or `"rgb888"`, for 24-bit frames from `frame()` and `capture()` |
| `frame_size` | `None` | The size `take()` returns, a `FrameSize`; `None` is the sensor's own ([below](#written-for-espcamera)) |
| `jpeg_quality` | `80` | `take()`'s JPEG quality, 1-100, higher is better |
| `raw` | `False` | Bypass the ISP and hand back the sensor's raw bytes |
| `lane_mbps` | `0` | Override the driver's MIPI lane bit rate; `0` trusts it |
| `exposure` | `None` | Initial exposure, 0–100 percent of the sensor's range |
| `gain` | `None` | Initial gain, 0–100. Not every driver has one — see `controls()` |
| `flip`, `mirror` | `False` | Initial orientation, upright by default |
| `bayer` | `None` | The ISP's colour order (`"BGGR"`, `"GBRG"`, `"GRBG"`, `"RGGB"`); `None` works it out from the sensor and the orientation |
| `awb` | `True` | Automatic white balance, from the ISP's own statistics |
| `test_pattern` | `False` | Start with the sensor's internal pattern |
| `sensor` | `None` | A sensor described in Python, used instead of probing ([below](#a-camera-described-in-python)) |
| `auto_exposure` | `None` | Steer exposure and gain to a brightness target, measured after gamma. On unless `False`; on a sensor with its own auto-exposure (the OV5647) it steers that loop's target |
| `black_level` | `None` | What the sensor reads with no light, in 8-bit steps, taken off before gamma |

Board facts are arguments rather than build-time constants on purpose: the
same firmware image should drive a camera on any board that has one, and a
pin number is not a reason to recompile.

#### Sharing the bus, which is the one that will bite you

On most boards the camera connector's SCCB lines are the panel's I2C bus,
shared with the touch controller and often the audio codecs. `i2c` must name
the port that bus is already on. Given a port that already has a bus,
`cameraif` attaches to it; given a free port, it opens its own.

Get this wrong and nothing reports an error. Both peripherals reach the pins
through the ESP32's pin matrix, `CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK` (set
by MicroPython upstream) suppresses the one check that would have caught it,
and the camera works perfectly — while every read of whatever else lives on
that bus times out. On this board that was the touchscreen, and the symptom
looked like a scheduler problem two layers away.

Two things guard it now. `cameraif` refuses to open a second master on pins
another driver has reserved, naming the port to pass instead. And
`machine.I2C` on the ESP32-P4 is built on esp-idf's new `i2c_master` driver
([patches/0002](patches/0002-micropython-esp32-p4-camera-sensor-sdkconfig.patch)),
because the legacy driver it used before cannot hand out a bus handle to
share and does not reserve its pins, so neither the sharing nor the check
was possible.

Those patches, and the one that adds the sensor-driver component to the
esp32 port, live in [patches/](patches/) with the module that needs them.
`./apply_patches.sh --apply [MP_DIR]` puts them on a MicroPython checkout
(`--status` and `--revert` too); the checkout defaults to `micropython`
beside this repository.

### Getting frames

**`frame(timeout=1000)` → `memoryview` | `None`**

The frame where it landed, with no copy. This is what a preview loop wants.

The view is borrowed, not given: it is valid until your next `frame()` or
`capture()` call on this camera, which hands the buffer back to the driver.
Copy anything you need to keep. The module holds two frame buffers and never
gives the DMA the one you are reading, so the picture will not tear under
you — but once released, the sensor will overwrite it within about 30 ms.

**`capture(buf=None, timeout=1000)`**

With a buffer, fills it and returns the byte count — the zero-allocation form
for a loop. With no buffer, returns a fresh `bytes`, for a one-shot still
where making the caller size 1.28 MB first is a poor introduction.

**`capture_scaled(dst, pic_w, pic_h, *, x=0, y=0, w=None, h=None, rotate=0, mirror=False, timeout=1000)`**

Scale the frame into a rectangle of a destination picture, using the P4's
Pixel Processing Accelerator through
[ppa](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/ppa),
so the firmware needs ppa too (`--modules cameraif,ppa`, or `all`). No pixel
passes through the CPU: the PPA reads the camera buffer and writes the
destination by DMA.

```python
fb = display_drv.framebuffers()[0]
cam.capture_scaled(fb, display_drv.width, display_drv.height)
display_drv.show()
```

The destination is described as a whole picture plus the block to write
inside it, because that is exactly what a panel framebuffer is — so a
preview hands this its scanout buffer and gets camera to glass in one
hardware operation. `rotate` takes 0, 90, 180 or 270 and `mirror` flips in x,
both free in the same pass. Returns the `(width, height)` written, or `None`
if no frame arrived.

`dst` should be cache-line aligned, since the PPA writes it by DMA. A panel
framebuffer already is. A misaligned buffer still gets the right picture, but
from ppa's software path, about 15 times slower. The PPA scales in sixteenths,
so a ratio it can't make exactly fills a few pixels less of the block rather
than leaving the preview to software.

**`capture_yuy2(buf, width, height, timeout=1000)`**

Converts to YUY2 and scales to `width` x `height` while copying, through ppa.
This exists because UVC — what a USB webcam speaks — wants YUY2, and doing
that conversion in Python is not viable. From the 800x800 sensor, 320x240
keeps the camera's 36 fps and 640x480 manages about 12, since neither ratio is
one the PPA makes and the scale runs in software.

**`capture_jpeg(quality=80, timeout=1000)` → `bytes` | `None`**

JPEG, encoded by the P4's hardware encoder through
[jpegio](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/jpegio),
so the firmware needs jpegio too (`--modules cameraif,jpegio`, or `all`);
without it this raises `OSError` and says so. The sensor's RGB565 goes to the
encoder untouched, so there is no software colour conversion in the path at
all. To encode any other RGB565 buffer the same way, use
`jpegio.JpegEncoder` directly. Measured on a normally-lit room at 800x800, against 1.28 MB raw:

| quality | size | ratio |
|---|---|---|
| 90 | ~144 KB | 8.9:1 |
| 80 | ~90 KB | 14:1 |
| 70 | ~69 KB | 18.5:1 |
| 50 | ~47 KB | 27:1 |

All four calls return `None` (or `0`) when no frame arrived inside the
timeout. At 35 fps a missed frame is a timing fact, not an error, and a
preview loop should simply ask again.

### Controls

First, ask what there is. Drivers differ enormously in how much of a sensor
they expose, and a program that assumes `gain()` exists works beautifully on
the sensor it was written for and raises `OSError` on the next one.

```python
>>> cam.controls()
{'exposure': (2, 235, 1), 'flip': (0, 1, 1), 'mirror': (0, 1, 1)}
```

`controls()` returns only what the driver in your firmware advertises, as
`{name: (minimum, maximum, step)}`. The example above is the real answer from
the OV5647 — three parameters, and no gain control at all.

Numeric controls take a **percentage of that advertised range**:

```python
cam.exposure(60)              # 60% of whatever this sensor offers
cam.exposure()                # read it back
cam.exposure(raw=True)        # (current, minimum, maximum, step), sensor units
cam.exposure(120, raw=True)   # set in sensor units
```

Percent is the default because the raw units are not portable: exposure is in
lines, gain is in driver-specific steps, and both ranges move when the format
does. A literal that suits one mode is quietly wrong in another. `raw=True`
is there for when you do mean the sensor's own numbers.

| Method | Effect |
|---|---|
| `controls()` | `{name: (min, max, step)}` for what this driver implements |
| `exposure(pct=None, *, raw=False)` | Shutter or AE target, depending on the driver |
| `gain(pct=None, *, raw=False)` | Gain. Tries absolute `GAIN`, falls back to analogue `ANGAIN` |
| `flip(on=None)` | Vertical flip, in the sensor. A bool. Also the `vflip` property |
| `mirror(on=None)` | Horizontal mirror, in the sensor. A bool. Also the `hmirror` property |
| `bayer()` | The colour order the ISP opened with |
| `awb(on=None)` | Automatic white balance. A bool |
| `wb_gains(red=None, blue=None)` | The white balance's red and blue gains (green is 1). Setting them turns `awb` off |
| `test_pattern(on=None)` | The sensor's internal pattern |
| `auto_exposure(on=None, *, target=None)` | `(on, target, brightness)`: exposure, then gain, moved every frame toward `target` (0–255, after gamma). Setting `exposure()` or `gain()` turns it off |
| `black_level(level=None)` | The black taken off each channel before gamma, allowing for white balance and the colour matrix |
| `ccm(matrix=None)` | The colour matrix, nine numbers row by row. On a P4 before revision 3 white balance scales its red and blue columns, so a highlight the sensor clips can take a tint; from revision 3 white balance runs in the ISP's own gain block, ahead of the clip |
| `gamma(exponent=None)` | The tone curve's exponent; below 1 lifts the mid-tones |

**Colour comes from the ISP, set up per sensor.** A raw sensor needs a colour
matrix (its filters overlap, so its colour is pale and green-leaning), a gamma
curve (it counts light linearly) and a white balance. The camera applies all
three: the matrix and gamma from the sensor's tuning (Espressif's, for the
OV5647; an identity matrix for a sensor without one), and the white balance
from the ISP's own statistics while `awb` is on.

**Flipping moves the colour order, and the camera follows it.** Either flip
moves the sensor's 2×2 colour cell, so the ISP's Bayer order changes with it,
whether you flip at open (`flip=`, `mirror=`) or on a running camera
(`flip()`, `mirror()`, `vflip`, `hmirror`). `bayer=` fixes the order instead.
The OV5647 draws its test pattern after the flip, so the pattern neither turns
over nor keeps its colours when you flip it; judge a flip by a real scene.

**Reading a control returns `None` when the driver cannot answer**, rather
than a number. This matters more than it looks: the OV5647 driver's
`get_para_value` returns `NOT_SUPPORTED` unconditionally, and an earlier
version of this module ignored that return code and handed back the zero it
had initialised — reporting a confident, plausible exposure of 0 for a
sensor that cannot report anything. Setting still works; only reading is
unavailable, and `None` is how it says so.

The flips and the `espcamera` properties are the exception: where the driver
can't read one back, they return what you last set (the camera sets both
flips when it opens), and `None` only for a control nobody has set.

`test_pattern()` reads back what was last *asked for*, not what the sensor
reports — the ioctl behind it has no read side. Said plainly here because a
reader who assumed otherwise would trust it during exactly the debugging
session it exists for.

It earns its place in the API. The pattern is generated inside the sensor,
after the pixel array and before everything else — the MIPI lanes, the ISP,
the DMA, the cache invalidate, your blit. If the pattern is clean and the
picture is not, the fault is in front of the sensor: optics, focus, light, a
lens cap. If the pattern is broken too, everything after the pixel array is
suspect. Either answer halves the search.

#### What the OV5647 driver actually supports

Read out of `esp_cam_sensor`'s driver source and confirmed on the board, in
case it saves someone the same hour:

| | set | read | notes |
|---|---|---|---|
| `exposure` | yes, 2–235 | no | `ov5647_set_AE_target` — an auto-exposure target, not a raw shutter time |
| `flip`, `mirror` | yes | no | Read back as last set |
| `gain` | **no** | no | The driver implements neither `GAIN` nor `ANGAIN` |
| `test_pattern` | yes | — | ioctl, set-only by design |

### Inspection

| Method | Returns |
|---|---|
| `size()` | `(width, height, bytes_per_frame)` |
| `controls()` | `{name: (min, max, step)}`, above |
| `sensor()` | `(name, product_id)` |
| `formats()` | `[(name, width, height), ...]` the driver offers |
| `reg(addr, value=None)` | Read or write one sensor register |
| `stats()` | `(frames, dropped, isr_new, isr_done, last_received)` |

`stats()` is the first thing to look at when there is no picture. If
`isr_done` is not climbing, nothing is arriving and the problem is upstream
of this module — power, clock, lanes, or the sensor not streaming. If it is
climbing and `frames` is not, frames are arriving and being rejected, and
`last_received` says how big they were.

### Teardown

```python
cam.deinit()
```

Idempotent, like the rest of PyDevices: calling it twice is not an error, and
neither is calling it on a camera that never opened. Afterwards every method
raises rather than touching freed hardware. `__del__` does the same on a
best-effort basis, and a soft reset (Ctrl-D) tears the camera down before the
heap is wiped — so an interrupted session leaves the CSI controller, the ISP,
the LDO and the sensor's stream all stopped, not half-running into memory
that no longer exists.

## Written for espcamera

CircuitPython's [`espcamera`](https://docs.circuitpython.org/en/latest/shared-bindings/espcamera/)
is the camera API CircuitPython code is written against, and on an ESP32-P4
this module answers to it. `import espcamera` gives you `cameraif`, with
espcamera's `take()`, `reconfigure()`, enums and properties. Only the
constructor differs, because espcamera's takes the DVP data pins of an
ESP32-S3 camera and this camera is on the CSI connector:

```python
import espcamera

# espcamera.Camera(data_pins=..., pixel_clock_pin=..., i2c=board.I2C(), ...) becomes
cam = espcamera.Camera(7, 8, i2c=1,
                       pixel_format=espcamera.PixelFormat.RGB565,
                       frame_size=espcamera.FrameSize.QVGA)
cam.vflip = True
frame = cam.take(1)                 # 320x240 RGB565

cam.reconfigure(pixel_format=espcamera.PixelFormat.JPEG, frame_size=espcamera.FrameSize.SVGA)
open("still.jpg", "wb").write(cam.take(1))
```

**`take(timeout=0.25)`** waits up to `timeout` seconds and returns the frame,
or `None`. In `RGB565` and `GRAYSCALE` that's the pixels: a read-only
`displayio.Bitmap` on CircuitPython, as espcamera returns, and a read-only
`memoryview` on MicroPython. It is borrowed, valid until the next `take()`.
In `JPEG` it's a `memoryview` of the encoded bytes, so
`isinstance(frame, memoryview)` tells the two apart on CircuitPython, as it
does with espcamera.

**`frame_size`** is what `take()` returns, not the sensor's mode, so
switching costs nothing: `reconfigure()` changes it on a running camera
without reopening anything. The sensor keeps the mode you opened it in
(`format=`, see `formats()`), and `take()` crops the frame to the
`FrameSize`'s shape and scales it on the P4's PPA. The crop is centred, at
the largest size the PPA scales exactly in hardware (it scales in
sixteenths). From the OV5647's 800x800, `SVGA` keeps 800x600 of the frame, and
`QVGA` and `VGA` keep 640x480. Left as `None`, `take()` returns the sensor's
own frame with no copy at all. None of it costs frames: on the OV5647 at
800x800, `take()` keeps the sensor's 35.7 fps natively, scaled to `QVGA`,
`HVGA`, `VGA` and `SVGA`, in `GRAYSCALE` at `QQVGA`, and as a JPEG natively
and at `SVGA`. `GRAYSCALE` and any `FrameSize` need
[ppa](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/ppa)
in the firmware, and `JPEG` needs
[jpegio](https://github.com/PyDevices/micropython-pydevices/tree/main/modules/jpegio).

**The controls are properties**, with espcamera's names, mapped onto the
`esp_cam_sensor` parameters that `controls()` reports:

| espcamera | `controls()` | What it reaches | On the OV5647 |
|---|---|---|---|
| `vflip`, `hmirror` | `flip`, `mirror` | `VFLIP`, `HMIRROR` | yes |
| `aec_value` | `exposure` | `EXPOSURE_VAL`, in the sensor's units | yes: its auto-exposure target, 2-235 |
| `agc_gain` | `gain` | `GAIN`, or `ANGAIN` | no |
| `brightness`, `contrast`, `saturation`, `sharpness`, `denoise` | the same | the parameter of that name | no |
| `whitebal` | `awb` | `AWB`; without one, the ISP's white balance (`awb()`) | yes, the ISP's |
| `gain_ctrl` | `agc` | `AGC` | no |
| `exposure_ctrl`, `ae_level`, `wb_mode`, `special_effect`, `lenc` | the same | `AE_CONTROL`, `AE_LEVEL`, `WB`, `SPECIAL_EFFECT`, `LENC` | no |
| `colorbar` | | the sensor's test pattern (`test_pattern()`) | yes |
| `quality` | | `take()`'s JPEG quality, 1-100 | yes |

A control the sensor lacks raises `AttributeError`, as espcamera does, naming
the sensor and pointing at `controls()`. So `hasattr(cam, "agc_gain")` is the
question to ask. `gain_ceiling`, `aec2`, `awb_gain`, `dcw`, `bpc`, `wpc` and
`raw_gma` have nothing behind them on any `esp_cam_sensor` driver and always
raise. Values are the sensor's own, as in espcamera, not the percentages that
`exposure()` and `gain()` take.

Also there: `pixel_format`, `frame_size`, `width` and `height` (of what `take()`
returns), `sensor_name`, `address`, `supports_jpeg`, `max_frame_size` (the
largest `FrameSize` the sensor's mode holds), `frame_available`, the
`GrabMode` and `GainCeiling` enums, and `with` blocks. `grab_mode` and
`framebuffer_count` are accepted by `reconfigure()`. The camera always keeps
two buffers and `take()` always returns the newest frame, so they read back as
`GrabMode.LATEST` and `2`.

What stays this module's own: the constructor, `frame()`, `capture()`,
`capture_jpeg()`, `capture_scaled()`, `capture_yuy2()`, `controls()` and the
ISP's tuning. Those names keep working as they did.

## Examples

| File | What it shows |
|---|---|
| [`camera_preview.py`](examples/camera_preview.py) | The camera on the board's own panel. No PC, no network, no USB |
| [`camera_espcamera.py`](examples/camera_espcamera.py) | espcamera's API: `take()`, `reconfigure()`, `vflip` and `hmirror`, a JPEG |
| [`camera_snapshot.py`](examples/camera_snapshot.py) | A still to the filesystem as hardware-encoded JPEG |
| [`camera_mjpeg.py`](examples/camera_mjpeg.py) | An MJPEG server any browser can open |
| [`camera_send_simpletest.py`](examples/camera_send_simpletest.py) | A JPEG on request to another board's screen or a computer, over TCP |
| [`camera_still.py`](examples/camera_still.py) | Live view, BOOT button takes the picture. The one that feels like a camera |
| [`camera_controls.py`](examples/camera_controls.py) | Discovering what a sensor supports, then sweeping it on screen |
| [`camera_frame.py`](examples/camera_frame.py) | Zero-copy `frame()` loop with `stats()` |
| [`camera_transform.py`](examples/camera_transform.py) | `capture_scaled` with rotate, mirror, and a cropped inset |
| [`camera_formats.py`](examples/camera_formats.py) | List `formats()`, then construct `Camera(...)` with a named format |
| [`camera_diag.py`](examples/camera_diag.py) | Bring-up: stats, test pattern, optional `reg()` |

The examples take the camera from `board_config`, so they run unchanged on
any board whose config provides one. On a board without one, construct a
`Camera` directly with your own pins -- see [`camera_formats.py`](examples/camera_formats.py).

[`camera_still.py`](examples/camera_still.py) is the one that uses `appdev` as the scheduler, which is
the house idiom for an application with input; the rest are plain scripts
because they have nothing to schedule.

USB webcam (the board presenting as UVC) lives in
usbhost's [`examples/usb_webcam.py`](../usbhost/examples/usb_webcam.py),
which sources frames from this module when a camera is present.

`available()` answers whether this firmware can open a camera at all: it is
true when at least one `esp_cam_sensor` driver was linked in, and false on a
build with no `CONFIG_CAMERA_<chip>` enabled and on every non-P4 target. It
does not say whether a sensor is plugged in -- that is what constructing a
`Camera` tells you, with a message.

## Performance, measured

On an ESP32-P4 with an OV5647 at 800x800 RGB565:

| | rate | what dominates |
|---|---|---|
| Sensor delivering frames | 36 fps | the format's own frame rate |
| `capture_jpeg(70)` | 36 fps | nothing — it keeps up with the sensor |
| `frame()` — zero copy | 35 fps | waiting for the next frame |
| `capture_scaled()` to the panel | 34 fps | the PPA, ~1 ms |
| Full preview loop, incl. `show()` | 18.6 fps | the panel's refresh, 24 ms |
| MJPEG over Wi-Fi, quality 40 | 15 fps | the network |
| MJPEG over Wi-Fi, quality 70 | 7 fps | the network |

Everything except the panel refresh runs at the rate the sensor delivers, and
`capture_jpeg()` matching it is not a rounding artefact — the hardware encoder
genuinely costs less than the 28 ms between frames. Two changes got it there, a
DMA scaler and a 97 ms wait that should not have existed:
[how it got there](docs/performance.md).

## Sending pictures over the network

[`camera_send_simpletest.py`](examples/camera_send_simpletest.py) sends one JPEG each time the viewer asks for one,
so every picture costs a round trip on the network as well as its bytes. On a
Waveshare ESP32-P4 WIFI6 DEV-KIT with an OV5647, sending 27 KB pictures at
quality 60 to CPython on a PC, 20 pictures a run:

| Link | per picture, median | slowest |
|---|---|---|
| Ethernet (100 Mbit, straight to the PC) | 62 ms, 55 of them waiting for the camera's next frame | 66 to 103 ms |
| Wi-Fi at -59 to -66 dBm, power saving off | 110 to 210 ms | 0.3 to 2.4 s |
| Wi-Fi at the same spot, power saving on (the default) | 300 to 690 ms | 0.7 to 7.9 s |
| Wi-Fi, power saving on, the receiver's window cut to 5,760 bytes | 920 ms | 4.8 s |

The time goes to the radio link, not to the code. `TCP_NODELAY` on the sender,
or the length and the JPEG in one write, made no difference on either link.
Power saving is the part you control: with it on, the P4's Wi-Fi sleeps between
the access point's beacons and each request waits for one, so the example turns
it off. A small receive window (the last row stands in for another
microcontroller as the viewer) means more round trips per picture, each of
them as slow. If pictures still crawl, move the board nearer the access point
or use Ethernet.

With another board as the viewer, the same DEV-KIT (at -61 to -65 dBm) sent to
a LilyGO T-Embed (ESP32-S3) running `jpegio_viewer_simpletest.py` at -83 to
-85 dBm, two runs of 20 pictures for each setting. The times are from the
viewer's request to the last byte of the JPEG. Decoding at an eighth of the
size and showing the picture took another 86 ms on the S3.

| Sender's power saving | Viewer's power saving | Bluetooth on the viewer | per picture, median | slowest |
|---|---|---|---|---|
| off | off | off | 108 to 152 ms | 1.3 to 1.5 s |
| off | on (the default) | off | 107 to 165 ms | 0.3 s |
| on (the default) | off | off | 327 to 365 ms | 0.7 to 1.5 s |
| on | on | off | 201 to 326 ms | 1.6 to 2.6 s |
| off | off | on | 502 to 536 ms | 2.6 to 3.5 s |
| on | on | on | 2.9 s | 7.4 s |

The sender's power saving matters and the viewer's barely does, so the example
turns it off on the sender only. Bluetooth matters most: the S3 shares one
radio between Bluetooth and Wi-Fi, and with Bluetooth active on the viewer
(here a Bluetooth REPL waiting for a connection) each picture took about four
times as long with power saving off and about ten times as long with it on.
Turn Bluetooth off on the viewer (`bluetooth.BLE().active(False)`) if nothing
needs it.

[`camera_mjpeg.py`](examples/camera_mjpeg.py) doesn't wait for requests, which is why it streams at 7 to
15 fps over the same Wi-Fi.

## Known hardware defect

The OV5647's `MIPI_CTRL00` register (0x4800) comes up with clock-lane gating
enabled, and the sensor then produces nothing over MIPI. `cameraif` writes
`0x34` to it for this sensor specifically, gated on its product ID.

The distinguishing bit is `CLOCK_LANE_GATE`. How that was established, and the
measurement trap that makes a naive sweep lie about it, are in the report:
[`docs/upstream-reports/ov5647-mipi-ctrl00.md`](docs/upstream-reports/ov5647-mipi-ctrl00.md),
filed upstream as
[esp-video-components#97](https://github.com/espressif/esp-video-components/issues/97).

What's planned next is in [ROADMAP.md](ROADMAP.md).

## Licence

MIT.

## A camera described in Python

A sensor that `esp_cam_sensor` doesn't ship needs no C. Describe it as a dict
and pass it as `sensor=`; `cameraif` opens it at its address, checks its chip
ID, and drives it from the description, just as it would one of Espressif's
drivers. The IMX219 is described this way, in pydevices'
[`drivers/camera/imx219.py`](https://github.com/PyDevices/pydevices/blob/main/drivers/camera/imx219.py):

```python
import cameraif
from imx219 import SENSOR

cam = cameraif.Camera(7, 8, i2c=1, reset=21, sensor=SENSOR)
```

| Key | Meaning |
|---|---|
| `name` | What `sensor()` reports |
| `addr`, `reg_bits` | The SCCB address, and 8- or 16-bit register addresses |
| `id` | `(register, value)`: the 16-bit chip ID, in that register and the next |
| `init` | `[(register, value), ...]` written before every mode; `(None, ms)` waits |
| `modes` | A list of dicts: `name`, `width`, `height`, `fps`, `format` (`"raw8"`, `"raw10"`, `"raw12"`), `bayer`, `lanes`, `lane_hz`, `pclk`, `hts`, `vts`, and the mode's `regs` |
| `stream` | `(registers on, registers off)` |
| `exposure`, `gain` | `(register, bytes, minimum, maximum, default)`; a maximum of `-n` is the mode's `vts` less `n`, a default of `None` the maximum |
| `flip` | `(register, vertical bits, horizontal bits)` |
| `test_pattern` | `(register, bytes, on value, off value)` |
| `auto_exposure`, `black_level`, `gamma`, `ccm` | The camera's tuning; arguments to `Camera()` override it |

Everything except `name`, `reg_bits`, the controls and the tuning is
required, and a missing or malformed key raises a `ValueError` naming it.
The description is copied when the camera opens, so changing the dict
afterwards does nothing.
