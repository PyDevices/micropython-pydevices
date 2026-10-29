# castif — live video from an ESP32-P4

`import castif` streams what your app draws to a screen across the room. You
hand it an RGB565 framebuffer. It scales and converts it with the PPA,
encodes H.264 in hardware, muxes MPEG-TS and sends it, all from a FreeRTOS
task on core 0. Your app keeps core 1 and its frame rate, and a long redraw
can't starve the stream.

There are two ways out:

- `castif.Hls` serves HLS over HTTP, for anything that plays it: VLC,
  ffplay, a browser with hls.js, a Roku.
- `castif.Cast` sends RTP to a Miracast (Wi-Fi Display) receiver, such as a
  TV, once your Python code has set up the session.

```python
import castif
hls = castif.Hls(640, 480, fps=20)
hls.start()                 # serving http://<board>:8090/stream.m3u8
while True:
    draw(frame)             # your drawing, into an RGB565 bytearray
    hls.offer(frame)        # skipped, not queued, if the encoder is busy
```

[`examples/castif_simpletest.py`](examples/castif_simpletest.py) draws a
moving square and serves it for VLC or ffplay to watch.

## Build

castif is ESP32-P4 only, and every other port and chip skips it. It calls
h264enc and tsmux through their C APIs, so build all three, plus ppa:
`--modules castif,h264enc,tsmux,ppa`, or `--modules all`. Without one of
them, `Cast()` and `Hls()` tell you which is missing.

CircuitPython builds it on the ESP32-P4 too. Its port doesn't drive the
boards' ESP32-C6 radio yet, so there's no network, and `start()` raises
`OSError`.

## Tests

[`tests/test_castif_hls.c`](tests/test_castif_hls.c) runs on a PC. It
compiles `Hls`'s segmenter, playlist and HTTP server against tsmux, feeds
them tsmux's canned stream, and fetches a segment over loopback. Then
[`tests/check_castif_hls.py`](tests/check_castif_hls.py) has ffprobe decode
every segment and the playlist it served. Each step's command is in
[`castif.yml`](../../.github/workflows/castif.yml), which runs both on every
change. The encoder task and the RTSP cast need an ESP32-P4.

## `castif.Hls(width, height, *, fps=20, bitrate=2_000_000, segments=3, target=900, port=8090, path=None)`

`start()` begins serving `/stream.m3u8` and its `seg<N>.ts` segments on
`port`, and on `<path>/stream.m3u8` when you give `path`. A TV caches by URL,
so give each run its own path. The task forces a keyframe every `target` ms
and cuts a segment at each one. It keeps the last eight segments in PSRAM and
lists the newest `segments` in the playlist.

`offer(framebuffer)` copies a finished frame into a back buffer and returns
True, or returns False while the task still holds the last one. Call it after
each frame you draw. The task never reads a frame you're still drawing.

`stats()` returns a dict that counts frames, offers, overruns and requests,
and times the PPA and the encoder. `stop()` stops serving. `close()` frees
the encoder.

## `castif.Cast(width, height, *, canvas_w=0, canvas_h=0, fps=30, gop=30, bitrate=3_000_000, qp_min=10, qp_max=45, out_size=0, audio=False)`

`start(framebuffer, sink_ip, dst_port, src_port)` begins sending, after the
receiver has answered RTSP PLAY. The Wi-Fi Display session (discovery and
RTSP) stays in Python, and is idle once PLAY is answered.

While it runs, you can call `force_idr()`, `set_bitrate(n)` and
`set_fps(n)`, and read counters from `stats()`. `stop()` ends it.

With `audio=True`, it carries a 48 kHz stereo LPCM track on the video's
clock. Feed it 10 ms blocks with `feed_audio()`, or give it an audiodsp
`audiopump.Tap` with `set_tap(tap, rate, channels)` and the task reads the
pump itself. The task keeps the track on the wall clock by dropping or
inserting one 10 ms block at a time, never by jumping.

The stream itself is tsmux's: see [`../tsmux/README.md`](../tsmux/README.md).
