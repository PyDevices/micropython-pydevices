# h264enc — H.264 on the ESP32-P4's hardware encoder

`import h264enc` turns RGB565 frames into H.264, on the P4's encoder in
silicon. Hand it any framebuffer; no camera or cast is involved.

```python
import h264enc
enc = h264enc.Encoder(720, 720, fps=30, gop=30, bitrate=3_000_000)
with open("clip.h264", "wb") as f:
    for frame in frames:                # each 720 * 720 * 2 bytes of RGB565
        f.write(enc.encode(frame))      # one access unit, Annex B
enc.close()
```

The file plays in ffmpeg and VLC as raw H.264 (`ffplay clip.h264`). Each
720x720 frame costs about 18 ms, the PPA's colour conversion and the encode
together, so the encoder keeps up with 30 fps.

ESP32-P4 only, and C only: the module is skipped on every other chip and port,
so `import h264enc` raises `ImportError` there. `--modules all` includes it;
name it with `--modules h264enc` otherwise. CircuitPython's ESP32-P4 boards
take it too (`--interpreter circuitpython --modules h264enc,ppa`), and
`build_mp.py` fetches Espressif's `esp_h264` for that build.

[`examples/h264enc_simpletest.py`](examples/h264enc_simpletest.py) encodes two seconds of a moving square to an H.264 file.

## API

### `h264enc.Encoder(width, height, fps=30, gop=30, bitrate=3_000_000, qp_min=10, qp_max=45, *, canvas_w=0, canvas_h=0, out_size=0, pipeline=False)`

`width` and `height` are multiples of 16, from 80x80 to 1920x2032. `gop` is
the frames from one IDR (key) frame to the next. With `canvas_w` / `canvas_h`
the frame is placed centred on a larger black picture, which is how a cast
puts a 720x720 panel on a 1280x720 TV. `out_size` caps one access unit. The
default holds a keyframe of the whole canvas at its raw size (1.4 MB at
1280x720, in PSRAM), because a busy picture such as a camera's can encode
bigger than any fixed guess, and a frame that doesn't fit stops the encoder.
`pipeline=True` overlaps the colour conversion of one frame with the encode
of the one before; see [Pipelined encoding](#pipelined-encoding).

The P4 has one H.264 encoder, so one `Encoder` (or one cast) at a time: a
second raises `OSError` saying it is busy, and the first carries on. `close()`
gives the encoder back; so does the object being collected, and `with` works.

### `encode(frame) -> bytes`

One frame of native-order RGB565 (what displaydev framebuffers hold), at least
`width * height * 2` bytes. Returns one access unit: Annex B NAL units, SPS
and PPS included before each IDR. A pipelined encoder returns the previous
frame's access unit instead, and `b""` for the first frame.

### `flush() -> bytes`

A pipelined encoder's last access unit, the one `encode()` hasn't returned
yet, or `b""` when there is none. A plain encoder always returns `b""`.

### `keyframe`, `force_idr()`, `set_bitrate(bps)`, `stats()`

`keyframe` says whether the access unit `encode()` (or `flush()`) last
returned is an IDR frame. `force_idr()` makes the next frame the encoder
starts an IDR (a viewer joining late needs one). `set_bitrate()` takes effect
from the next frame. `stats()` is a dict: `frames` and `keyframes` returned so
far, `bytes`, `errors`, the last conversion's `ppa_us` and the last encode's
`enc_us`, and `wall_us`, how long the last `encode()` kept its caller.
`pipeline` says which kind of encoder this is. `close()` returns what
`flush()` would have, or `None` for a plain encoder; leaving a `with` block
discards it, so flush first.

## Pipelined encoding

The colour conversion runs on the PPA and the encode on the H.264 engine, two
separate pieces of hardware. A plain encoder runs them one after the other, so
a frame costs their sum: at 1920x1072 about 40 ms to convert and 38 ms to
encode, 12 frames a second. A pipelined encoder converts frame N on the PPA
while frame N-1 is still being encoded, so a frame costs about the longer of
the two.

```python
enc = h264enc.Encoder(1920, 1072, fps=25, gop=25, pipeline=True)
with open("clip.h264", "wb") as f:
    for frame in frames:
        f.write(enc.encode(frame))     # the previous frame's access unit
    f.write(enc.flush())               # and the last one
enc.close()
```

The price is one frame of latency: each `encode()` returns the frame before.
`encode()` has finished reading `frame` when it returns, so you can capture the
next picture into the same buffer straight away. The encoder keeps two YUV
canvases in PSRAM rather than one (3 MB each at 1920x1088) and starts one
small task the first time a pipelined encoder opens, and keeps it.

It's off by default because the latency changes what `encode()` returns, and
code that pairs each frame with its own access unit (writing timestamps, or
checking `keyframe` straight after) would quietly be off by one.

The PPA does one operation at a time. Any other `ppa.srm()` call, such as a
preview cropping and scaling each camera frame for a screen, waits while the
encoder's conversion runs, and the conversion waits for it. A preview's PPA time
adds to the conversion's, so with a preview running at 1080p the PPA, not
the encoder, sets the frame rate. `stats()["ppa_us"]` and your own preview's
timing show how close the two together come to the frame period.

## Colour

The PPA converts to limited-range BT.601 YUV 4:2:0, which is what an H.264
decoder assumes when the stream does not say. The encoder reads that as packed
YUV 4:2:0, the only raw format the pre-revision-3 P4 accepts.

## For C

[`src/h264enc.h`](src/h264enc.h) is the same session for native code: `h264enc_open()`,
`h264enc_encode()` into the session's own buffer, `h264enc_force_idr()`,
`h264enc_set_bitrate()`, `h264enc_close()`. castif's core-0 cast task encodes
through it, declaring the functions weak so castif still links without
h264enc.

## Tests

[`tests/hardware/test_h264enc.py`](tests/hardware/test_h264enc.py) runs on a P4 with [`tests/h264_frames.py`](tests/h264_frames.py) beside it:
60 frames of a moving box, IDR placement, `force_idr()`, the busy rule. It
writes `h264enc.h264`, and [`tests/check_h264_ffmpeg.py`](tests/check_h264_ffmpeg.py) has ffmpeg on the PC
decode it: every frame, at 30 dB or better against the source (33.3 dB
measured), and a frame-late comparison that must fail.

Without a board, [`tests/encode_h264_ffmpeg.py`](tests/encode_h264_ffmpeg.py) has ffmpeg's x264 encode the
same frames at the same settings, for [`check_h264_ffmpeg.py`](tests/check_h264_ffmpeg.py) to score.
[`h264enc.yml`](../../.github/workflows/h264enc.yml) runs both on every change, and builds a P4 firmware
with h264enc to check it compiles and links.

## Build

[`components/h264enc_esp_h264/`](components/h264enc_esp_h264/) is an ESP-IDF component with no sources whose
[`idf_component.yml`](components/h264enc_esp_h264/idf_component.yml) fetches Espressif's `esp_h264` (1.4.1);
[`build_mp.py`](../../build_mp.py) adds it to the build whenever h264enc is selected. It lived in
castif until the media roadmap's Phase 3 (2026-10-06).
