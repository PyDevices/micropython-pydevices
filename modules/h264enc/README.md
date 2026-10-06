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
name it with `--modules h264enc` otherwise.

## API

### `h264enc.Encoder(width, height, fps=30, gop=30, bitrate=3_000_000, qp_min=10, qp_max=45, *, canvas_w=0, canvas_h=0, out_size=0)`

`width` and `height` are multiples of 16, from 80x80 to 1920x2032. `gop` is
the frames from one IDR (key) frame to the next. With `canvas_w` / `canvas_h`
the frame is placed centred on a larger black picture, which is how a cast
puts a 720x720 panel on a 1280x720 TV. `out_size` caps one access unit (the
default, 256 KB, holds any frame at these bitrates).

The P4 has one H.264 encoder, so one `Encoder` (or one cast) at a time: a
second raises `OSError` saying it is busy, and the first carries on. `close()`
gives the encoder back; so does the object being collected, and `with` works.

### `encode(frame) -> bytes`

One frame of native-order RGB565 (what displaydev framebuffers hold), at least
`width * height * 2` bytes. Returns one access unit: Annex B NAL units, SPS
and PPS included before each IDR.

### `keyframe`, `force_idr()`, `set_bitrate(bps)`, `stats()`

`keyframe` says whether the last `encode()` made an IDR frame. `force_idr()`
makes the next one an IDR (a viewer joining late needs one). `set_bitrate()`
takes effect from the next frame. `stats()` is a dict: `frames`, `keyframes`,
`bytes`, `errors`, and the last frame's `ppa_us` and `enc_us`.

## Colour

The PPA converts to limited-range BT.601 YUV 4:2:0, which is what an H.264
decoder assumes when the stream does not say. The encoder reads that as packed
YUV 4:2:0, the only raw format the pre-revision-3 P4 accepts.

## For C

`src/h264enc.h` is the same session for native code: `h264enc_open()`,
`h264enc_encode()` into the session's own buffer, `h264enc_force_idr()`,
`h264enc_set_bitrate()`, `h264enc_close()`. castif's core-0 cast task encodes
through it, declaring the functions weak so castif still links without
h264enc.

## Tests

`tests/test_h264enc.py` runs on a P4 with `tests/h264_frames.py` beside it:
60 frames of a moving box, IDR placement, `force_idr()`, the busy rule. It
writes `h264enc.h264`, and `tests/check_h264_ffmpeg.py` has ffmpeg on the PC
decode it: every frame, at 30 dB or better against the source (33.3 dB
measured), and a frame-late comparison that must fail.

## Build

`components/h264enc_esp_h264/` is an ESP-IDF component with no sources whose
`idf_component.yml` fetches Espressif's `esp_h264` (1.4.1);
`build_mp.py` adds it to the build whenever h264enc is selected. It lived in
castif until the media roadmap's Phase 3 (2026-10-06).
