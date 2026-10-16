# Board half of the sound card through audiodsp's pump: usb-device-audio's
# speaker writes into an audiopump.Ring from the USB interrupt
# (Ring.from_usb), and audiodsp's pump plays that ring on the board's I2S
# wire through a BusioDriver. Python
# only steers the clock feedback.
import time

import audiopump
import usb.device
from audiodev import AudioFormat
from usb.device.audio import SpeakerInterface

ring = None
spk = None


def start(block_frames=480, blocks=8):
    global ring, spk
    ring = audiopump.Ring(sample_rate=48000, channel_count=2, frames=block_frames, capacity=blocks)
    spk = SpeakerInterface(sink=ring)
    usb.device.get().init(spk, builtin_driver=True)
    return spk.packet


def _driver(fmt, volume):
    # The pump plays only through a driver on the board's I2S wire.
    from audiodev import pump
    from boarddev import pcm_out

    transport = pcm_out(fmt)
    while transport is not None and getattr(transport, "wire", None) is None:
        transport = getattr(transport, "_inner", None)
    return pump, pump.BusioDriver(transport.wire, fmt, power=getattr(transport, "audio_power", None),
                                  volume=volume, transport=transport)


def play(seconds, volume=85):
    pump, driver = _driver(AudioFormat(48000, 2, 16), volume)
    if not pump.owner().play(spk, ring, driver=driver, raises=True):
        raise RuntimeError(pump.owner().fault())
    fb_lo = fb_hi = None
    levels = []
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    try:
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            spk.service()
            if spk.streaming:
                fb = spk._fb
                fb_lo = fb if fb_lo is None or fb < fb_lo else fb_lo
                fb_hi = fb if fb_hi is None or fb > fb_hi else fb_hi
                levels.append(ring.level())
            time.sleep_ms(5)
    finally:
        pump.owner().stop(spk)
    levels.sort()
    stream, stats, fb = spk.stats()
    return {"stream": stream, "ring_stats": stats[:6], "feedback": (fb_lo, fb_hi, spk._nominal),
            "level_median": levels[len(levels) // 2] if levels else None}
