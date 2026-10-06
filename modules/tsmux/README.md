# tsmux — MPEG-TS and HLS from H.264

`import tsmux` wraps H.264 access units in MPEG-TS, and cuts them into HLS
segments with a playlist, so a board can serve live video to anything that
plays HLS: a Roku (the Companion channel's video mode), VLC, a browser with
hls.js. Portable C, on every port.

```python
import h264enc, tsmux
enc = h264enc.Encoder(1280, 720, 20, 20)        # a keyframe every second
hls = tsmux.Segmenter(segments=3, target=1000)  # 1 s segments, last 3 kept
while True:
    au = enc.encode(framebuffer)
    hls.add(au, pts_90khz, enc.keyframe)
    # serve hls.playlist() as stream.m3u8 and hls.segment(n) as seg<n>.ts
```

`examples/roku/roku_companion_p4_video.py` in pydevices-examples is that loop
whole, with the HTTP server, playing on a TV.

## API

### `tsmux.Segmenter(segments=3, target=1000)`

`add(au, pts, key)` takes one access unit (Annex B, as h264enc returns it),
its PTS in 90 kHz ticks, and whether it is a keyframe; it returns True when it
finished a segment. A segment starts at a keyframe, ends at the first keyframe
at least `target` ms later, and begins with the PAT and PMT, so each plays on
its own. The last `segments` finished ones are kept (1 to 16).

`playlist(prefix="seg")` is the HLS media playlist of the segments held, as a
str: `#EXT-X-MEDIA-SEQUENCE` counts up as old segments go, so a player
following it live sees a moving window. `segment(n)` is segment `n`'s bytes,
or None once it has gone (or before it exists). `first` and `count` say which
are held.

For the Roku, 1-second segments and 3 kept give the least delay that still
plays steadily, about 7-10 s (measured from a PC before the P4 could do it:
pydevices-examples `spikes/roku_hls/FINDINGS.md`).

### `tsmux.Muxer(*, lpcm=False)`

The muxer underneath, one call at a time, each returning TS packets as bytes:
`tables()` (the PAT and PMT, before each keyframe), `video(au, pts, key=False)`,
`lpcm(pcm, pts)` (48 kHz stereo s16, native order; needs `lpcm=True`),
`pcr(pcr)` (a clock reference alone, for a tick with no frame) and `reset()`.

The stream is the one castif sends to Miracast receivers: program 1, the PMT
on PID 0x1000, H.264 on 0x100 (the PCR PID, the PCR 400 ms ahead of the PTS),
an access-unit delimiter before each frame, and LPCM on 0x101 as HDMV stream
type 0x83. LPCM is not an HLS audio format: a Roku plays it silent.

## For C

`src/tsmux_core.h` is the muxer with no MicroPython in it, writing each
188-byte packet to a callback. castif's cast task uses it on core 0 through
weak symbols, so castif still links without tsmux.

## Tests

`tests/test_tsmux.py` (unix MicroPython) muxes `tests/canned_160x96.h264`
(90 frames from ffmpeg) into a stream and HLS, and `tests/check_tsmux_ffmpeg.py`
has ffmpeg decode the stream, each segment alone and the playlist.
`tests/compare_castif.c` builds castif's muxer as it was (extracted verbatim
from git by `tests/extract_castif_ref.py`) beside tsmux and checks they write
the same bytes: 86,000 packets, every packet-boundary size, with and without
LPCM. CI runs all three (`.github/workflows/tsmux.yml`).
