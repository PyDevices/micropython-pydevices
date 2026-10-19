"""audioif#5: do the T-Embed's two I2S ports drift, over an hour?

Forty seconds said no -- `rx - tx` stayed at 0 bytes at a 4 x 128 ring. Two
clock dividers off one PLL are handed the same divisor but nothing LOCKS
them the way one channel pair locks BCLK and WS, so forty seconds is not an
answer for a stompbox somebody leaves on. This is the long run.

    mpftp put -d COM12 s3_drift.py /lib/s3_drift.py
    mpftp run -d COM12 <driver>          # no --follow: it takes an hour
    mpftp get -d COM12 /s3_drift.txt

The speaker stays SILENT on purpose. Both DMA counters advance at their own
port's frame clock whatever the wire carries -- the TX DMA clocks the zeros
`auto_clear` leaves when nothing is written, and the RX DMA clocks in
whatever the ES7210 sends -- so a drift measurement needs no sound, and the
microphone is two inches from the amplifier. The pump is spawned anyway, on
a near-silent sample, so the run is a stompbox's load rather than two idle
peripherals.

Nothing is written to the filesystem while it runs: a flash write costs this
board 43-48 ms of stopped audio. Every line is held and the file is written
once, at the end, after the pump is down.
"""
import gc
import struct
import time
from array import array

import audiocore
import audiopump
import _audioif
import board_peripherals as bp

RATE = 48000
CH = 2
FRAME = 2 * CH
BLOCKS = 0x7FFFFFFF

_lines = []


def _p(*a):
    s = " ".join(str(x) for x in a)
    _lines.append(s)
    print(s)


def _quiet(frames=2048, amp=64):
    """A near-silent tone: something real to pump, nothing to hear."""
    data = array("h")
    for i in range(frames):
        # a triangle at about 23 Hz, peak `amp` of 32767 -- about -54 dBFS
        v = (i * 4 * amp) // frames
        if v > 2 * amp:
            v = 4 * amp - v
        v -= amp
        data.append(v)
        data.append(v)
    return audiocore.RawSample(data, sample_rate=RATE, channel_count=CH)


def drift(seconds=3600, report=60, dma_desc=4, dma_frame=128,
          path="/s3_drift.txt"):
    del _lines[:]
    gc.collect()
    # No PWM MCLK bootstrap here, and that is the point rather than an
    # omission: `board_peripherals._ensure_in_mclk` exists because this
    # firmware's `machine.I2S` has no `mck=`, so the ES7210's 256fs clock has
    # to come from a PWM. `_audioif.i2s_start` takes `in_mclk` and the I2S
    # peripheral generates it on the same pin -- driving both would be two
    # sources on GPIO48.
    # The first five are positional: port, bclk, ws, dout, rate.
    depth = _audioif.i2s_start(
        bp._OUT_PORT, bp._IIS_BCLK, bp._IIS_WCLK, bp._IIS_DOUT, RATE,
        bits=16, channels=CH, mclk=-1,
        dma_desc=dma_desc, dma_frame=dma_frame,
        din=bp._ES7210_DIN, in_port=bp._IN_PORT,
        in_bclk=bp._ES7210_BCLK, in_ws=bp._ES7210_LRCK,
        in_mclk=bp._ES7210_MCLK)
    sample = _quiet()
    st = bytearray(audiopump.STATUS_BYTES)
    gc.collect()
    tx0 = _audioif.i2s_dma_bytes()
    rx0 = _audioif.i2s_rx_bytes()
    t0 = time.ticks_ms()
    core = audiopump.spawn(sample, BLOCKS, st, sink=True, loop=True,
                           timeout_ms=500)
    _p("s3_drift: %ds, ring %dx%d (%d bytes), pump on core %s, tx port %s "
       "rx port %s" % (seconds, dma_desc, dma_frame, depth, core,
                       bp._OUT_PORT, bp._IN_PORT))
    _p("    s        tx_bytes        rx_bytes      rx-tx   tx_err%   rx_err%"
       "   starved")
    elapsed = 0.0
    while elapsed < seconds:
        left = seconds - elapsed
        time.sleep(report if left > report else left)
        elapsed = time.ticks_diff(time.ticks_ms(), t0) / 1000.0
        tx = _audioif.i2s_dma_bytes() - tx0
        rx = _audioif.i2s_rx_bytes() - rx0
        want = elapsed * RATE * FRAME
        starved = (_audioif.i2s_starved_bytes()
                   if hasattr(_audioif, "i2s_starved_bytes") else -1)
        _p("%5d %15d %15d %10d %+9.4f %+9.4f %9d"
           % (elapsed, tx, rx, rx - tx,
              100.0 * (tx - want) / want, 100.0 * (rx - want) / want,
              starved))
    w = struct.unpack("<%dQ" % audiopump.STATUS_WORDS, st)
    audiopump.shutdown()
    _audioif.i2s_stop()
    _p("blocks=%d err=%d fault=%d sink_timeouts=%d worst_pull=%dus"
       % (w[0], w[5], w[24], w[14], w[20]))
    tx = _audioif.i2s_dma_bytes() - tx0
    rx = _audioif.i2s_rx_bytes() - rx0
    _p("FINAL after %.1f s: rx-tx = %d bytes = %.3f frames = %.3f ms"
       % (elapsed, rx - tx, (rx - tx) / float(FRAME),
          (rx - tx) * 1000.0 / (RATE * FRAME)))
    with open(path, "w") as f:
        f.write("\n".join(_lines) + "\n")
    gc.collect()
    return rx - tx
