"""Does a flash erase stall the I2S DMA itself, or only the pump?

    mpftp put -d COM12 tests/s3_stall_witness.py /lib/s3_stall_witness.py
    mpftp exec -d COM12 "import s3_stall_witness as w; w.run()"

This is the probe that settled how `I2SOut.starved()` has to be counted
(audioif#8), and the answer is not the one the issue assumed.


If only the pump stalls, the DMA keeps clocking `auto_clear`'s zeros and
`i2s_dma_bytes()` runs PAST what was fed -- a deficit any DMA-side counter
can see. If the DMA stalls too, there is no deficit to see and the only
witness is the byte clock against the wall.
"""
import time
import _audioif
import s3_busio as b

RATE = 48000
FRAME = 4


def run(seconds=6.0, kb=32):
    i2s = b.out()
    i2s.play(b.raw(220, rate=RATE), loop=True)
    time.sleep(0.5)
    t0 = time.ticks_ms()
    d0 = _audioif.i2s_dma_bytes()
    s0 = _audioif.i2s_sink_bytes()
    print(" phase        ms      dma     sink  sink-dma  starved   due-dma")
    blob = bytes(1024)

    def row(tag):
        ms = time.ticks_diff(time.ticks_ms(), t0)
        dma = _audioif.i2s_dma_bytes() - d0
        sink = _audioif.i2s_sink_bytes() - s0
        due = ms * RATE * FRAME // 1000
        print("%-10s %7d %8d %8d %9d %8d %9d"
              % (tag, ms, dma, sink, sink - dma, i2s.starved(), due - dma))

    row("before")
    with open("/s3_stallwho.bin", "wb") as f:
        for i in range(kb):
            f.write(blob)
            f.flush()
    row("after-wr")
    time.sleep(1.0)
    row("+1s")
    time.sleep(1.0)
    row("+2s")
    st = i2s.starved()
    i2s.stop()
    i2s.deinit()
    try:
        import os
        os.remove("/s3_stallwho.bin")
    except OSError:
        pass
    return st
