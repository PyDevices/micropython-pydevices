# Board half of the sound card through audiodsp's pump: usb-device-audio's
# speaker writes into an audiopump.Ring from the USB interrupt
# (Ring.from_usb), and audiodsp's pump plays that ring on the board's I2S
# wire through a BusioDriver. Python only steers the clock feedback.
import time

import machine

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
    marks = []
    usbd = machine.USBDevice()
    end = time.ticks_add(time.ticks_ms(), int(seconds * 1000))
    try:
        while time.ticks_diff(end, time.ticks_ms()) > 0:
            spk.service()
            if spk.streaming:
                fb = spk._fb
                fb_lo = fb if fb_lo is None or fb < fb_lo else fb_lo
                fb_hi = fb if fb_hi is None or fb > fb_hi else fb_hi
                levels.append(ring.level())
                st = usbd.stream_stats(spk.ep_out)
                if st is not None and (not marks or time.ticks_diff(time.ticks_us(), marks[-1][0]) >= 250_000):
                    marks.append((time.ticks_us(), st[0], st[2]))
            time.sleep_ms(5)
    finally:
        pump.owner().stop(spk)
    levels.sort()
    _, stats, fb = spk.stats()
    # Loss: the host sends one packet every (micro)frame while it streams, so
    # packets received against the frames that passed is what arrived. Taken
    # over the middle of the stream, a quarter-second at a time, away from
    # the start and the end, where the host isn't sending yet or any more.
    loss = None
    mid = marks[2:-2]
    if len(mid) >= 2:
        per_ms = 8 if spk.high_speed else 1
        worst = 0
        for a, b in zip(mid, mid[1:]):
            frames = time.ticks_diff(b[0], a[0]) * per_ms / 1000
            worst = max(worst, frames - (b[1] - a[1]))
        frames = time.ticks_diff(mid[-1][0], mid[0][0]) * per_ms / 1000
        got = mid[-1][1] - mid[0][1]
        loss = {"frames": round(frames), "packets": got, "lost_pct": round(100 * (1 - got / frames), 3),
                "worst_quarter_missing": round(worst), "drops": mid[-1][2] - mid[0][2]}
    return {"loss": loss, "ring_stats": stats[:6], "feedback": (fb_lo, fb_hi, spk._nominal),
            "level_median": levels[len(levels) // 2] if levels else None}
