"""tsmux on unix MicroPython -- the media Phase 4 portable half.

Muxes canned_160x96.h264 (90 frames, a keyframe every 30, from ffmpeg) into
OUT/stream.ts with tsmux.Muxer, and into HLS with tsmux.Segmenter (1 s
segments, 3 kept): OUT/seg<n>.ts and OUT/stream.m3u8. Checks what it can
here -- packet sync, continuity counters, segment starts, the playlist --
and check_tsmux_ffmpeg.py has ffmpeg decode the files on the PC.

    micropython test_tsmux.py [--out DIR]
"""

import os
import sys

import tsmux

HERE = __file__.rsplit("/", 1)[0] if "/" in __file__ else "."
ARGV = getattr(sys, "argv", [])[1:]
OUT = ARGV[ARGV.index("--out") + 1] if "--out" in ARGV else HERE + "/out"
AUD = b"\x00\x00\x00\x01\x09"


def access_units(data):
    """Split an Annex B stream at its access-unit delimiters (dropped: tsmux adds its own)."""
    starts = []
    i = data.find(AUD)
    while i >= 0:
        starts.append(i)
        i = data.find(AUD, i + 1)
    aus = []
    for k, s in enumerate(starts):
        end = starts[k + 1] if k + 1 < len(starts) else len(data)
        body = data[s + len(AUD) + 1:end]          # skip the AUD and its one payload byte
        aus.append(body)
    return aus


def is_key(au):
    i = au.find(b"\x00\x00\x01")
    while i >= 0:
        if au[i + 3] & 31 == 5:
            return True
        i = au.find(b"\x00\x00\x01", i + 3)
    return False


def check_packets(ts, label):
    """Every packet starts 0x47, and each PID's counter steps by one per payload."""
    assert len(ts) % 188 == 0, label
    cc = {}
    for off in range(0, len(ts), 188):
        p = ts[off:off + 188]
        assert p[0] == 0x47, "%s: no sync byte at packet %d" % (label, off // 188)
        pid = ((p[1] & 0x1F) << 8) | p[2]
        afc = (p[3] >> 4) & 3
        c = p[3] & 15
        if afc & 1:          # has payload: the counter advances
            if pid in cc:
                assert c == (cc[pid] + 1) & 15, "%s: PID %x counter %d after %d" % (label, pid, c, cc[pid])
            cc[pid] = c
    return len(ts) // 188


def main():
    try:
        os.mkdir(OUT)
    except OSError:
        pass
    with open(HERE + "/canned_160x96.h264", "rb") as f:
        aus = access_units(f.read())
    keys = [i for i, au in enumerate(aus) if is_key(au)]
    print("canned stream: %d access units, keyframes at %s" % (len(aus), keys))
    assert len(aus) == 90 and keys == [0, 30, 60]

    m = tsmux.Muxer()
    ts = bytearray()
    for i, au in enumerate(aus):
        key = i in keys
        if key:
            ts += m.tables()
        ts += m.video(au, 90000 + i * 3000, key)
    n = check_packets(ts, "stream.ts")
    with open(OUT + "/stream.ts", "wb") as f:
        f.write(ts)
    print("stream.ts: %d packets, sync and continuity good" % n)

    seg = tsmux.Segmenter(3, 1000)
    finished = 0
    for i, au in enumerate(aus):
        finished += seg.add(au, 90000 + i * 3000, i in keys)
    # 0..29, 30..59 finished by keyframes 30 and 60; 60..89 is still open
    print("segmenter: %d finished, first %s, count %d" % (finished, seg.first, seg.count))
    assert finished == 2 and seg.first == 0 and seg.count == 2
    pl = seg.playlist()
    print(pl)
    assert pl.startswith("#EXTM3U\n") and "#EXT-X-TARGETDURATION:1\n" in pl
    assert "#EXT-X-MEDIA-SEQUENCE:0\n" in pl and "#EXTINF:1.000,\nseg0.ts\n" in pl and "seg1.ts" in pl
    for k in (0, 1):
        s = seg.segment(k)
        check_packets(s, "seg%d.ts" % k)
        assert s[:3] == b"\x47\x40\x00", "seg%d does not start with the PAT" % k
        with open(OUT + "/seg%d.ts" % k, "wb") as f:
            f.write(s)
    assert seg.segment(2) is None and seg.segment(-1) is None
    with open(OUT + "/stream.m3u8", "w") as f:
        f.write(pl)

    # keep only 3: after more keyframes the oldest go. And the target duration
    # never changes while the stream plays (HLS forbids it; a Roku and VLC
    # dropped a playlist whose value flipped between 1 and 2)
    # The second round's frames come 10 % slow, so its segments last 1.1 s:
    # a target taken from the segments held would flip 1 -> 2 -> 1 here.
    targets = set()
    pts = 90000 + 90 * 3000
    for r in range(1, 4):
        for i, au in enumerate(aus):
            pts += 3300 if r == 2 else 3000
            if seg.add(au, pts, i in keys):
                line = [ln for ln in seg.playlist().split("\n") if ln.startswith("#EXT-X-TARGETDURATION:")]
                targets.add(line[0])
    assert len(targets) == 1, targets
    print("after 12 s: first %d, count %d" % (seg.first, seg.count))
    assert seg.count == 3 and seg.first == 8 and seg.segment(7) is None and seg.segment(10) is not None
    assert "#EXT-X-MEDIA-SEQUENCE:8\n" in seg.playlist()

    # the LPCM track castif carries
    a = tsmux.Muxer(lpcm=True)
    pcm = bytes(range(256)) * 7 + bytes(1920 - 7 * 256)
    tsa = a.tables() + a.video(aus[0], 90000, True) + a.lpcm(pcm, 90000) + a.pcr(54000)
    check_packets(tsa, "lpcm")
    try:
        tsmux.Muxer().lpcm(pcm, 0)
        raise AssertionError("lpcm() without lpcm=True")
    except ValueError:
        pass
    print("tsmux tests passed")


main()
