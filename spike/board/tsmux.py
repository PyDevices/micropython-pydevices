"""MPEG-2 transport stream packetizer for a live H.264 stream, plus LPCM audio.

Written for MicroPython (runs unchanged on CPython). One PAT, one PMT, one
video PID (0x1B, Annex-B H.264) and optionally one audio PID (0x83 HDMV LPCM
48 kHz 16-bit stereo, or 0x0F ADTS AAC). Every 188-byte packet goes to
``out.push(pkt)`` (an RtpOut, or anything with push()).
"""
import struct

PID_PMT = 0x1000
PID_VIDEO = 0x100
PID_AUDIO = 0x101
AUD = b"\x00\x00\x00\x01\x09\xf0"


def _crc_table():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if (c & 0x80000000) else (c << 1)
        t.append(c & 0xFFFFFFFF)
    return t


_CRC = _crc_table()


def crc32_mpeg(data):
    c = 0xFFFFFFFF
    for b in data:
        c = ((c << 8) & 0xFFFFFFFF) ^ _CRC[((c >> 24) ^ b) & 0xFF]
    return c


def _section(table_id, ident, body):
    length = 5 + len(body) + 4
    head = struct.pack(">BHHBB", table_id, 0xB000 | length, ident, 0xC1, 0) + body
    # 0xB000: section_syntax_indicator=1, '0', reserved '11', then the 12-bit length
    return head + struct.pack(">I", crc32_mpeg(head[:]))


class TsMux:
    def __init__(self, audio=None):
        self.cc = [0] * 8192
        pat_body = struct.pack(">HH", 1, 0xE000 | PID_PMT)
        self.pat = _section(0x00, 1, pat_body)
        streams = struct.pack(">BHH", 0x1B, 0xE000 | PID_VIDEO, 0xF000)
        if audio == "lpcm":
            streams += struct.pack(">BHH", 0x83, 0xE000 | PID_AUDIO, 0xF004) + b"\x83\x02\x46\x2f"
        elif audio == "aac":
            streams += struct.pack(">BHH", 0x0F, 0xE000 | PID_AUDIO, 0xF000)
        pmt_body = struct.pack(">HH", 0xE000 | PID_VIDEO, 0xF000) + streams
        self.pmt = _section(0x02, 1, pmt_body)
        self.pkt = bytearray(188)
        self.mv = memoryview(self.pkt)
        self.pes_video = bytearray(14)
        self.pes_video[0:6] = b"\x00\x00\x01\xe0\x00\x00"
        self.pes_video[6] = 0x84
        self.pes_video[7] = 0x80
        self.pes_video[8] = 0x05
        self.packets = 0

    def _header(self, pid, pusi, afc):
        pkt = self.pkt
        cc = self.cc[pid]
        self.cc[pid] = (cc + 1) & 15
        pkt[0] = 0x47
        pkt[1] = (0x40 if pusi else 0) | (pid >> 8)
        pkt[2] = pid & 0xFF
        pkt[3] = (afc << 4) | cc

    def tables(self, out):
        """PAT and PMT, one packet each."""
        for pid, sec in ((0, self.pat), (PID_PMT, self.pmt)):
            self._header(pid, 1, 1)
            self.pkt[4] = 0
            n = len(sec)
            self.mv[5:5 + n] = sec
            for i in range(5 + n, 188):
                self.pkt[i] = 0xFF
            out.push(self.mv)
            self.packets += 1

    @staticmethod
    def _pts(buf, off, pts):
        buf[off] = 0x21 | ((pts >> 29) & 0x0E)
        buf[off + 1] = (pts >> 22) & 0xFF
        buf[off + 2] = 0x01 | ((pts >> 14) & 0xFE)
        buf[off + 3] = (pts >> 7) & 0xFF
        buf[off + 4] = 0x01 | ((pts << 1) & 0xFE)

    def _pes(self, pid, pes_header, payload, pcr, out):
        """One PES packet (header bytes + payload) as TS packets, PCR on the first."""
        pkt = self.pkt
        mv = self.mv
        hlen = len(pes_header)
        total = hlen + len(payload)
        pos = 0        # bytes of (header+payload) consumed
        first = True
        while pos < total:
            room = 184
            body = 4
            if first:
                self._header(pid, 1, 3)
                pkt[4] = 7
                pkt[5] = 0x10
                base = pcr
                pkt[6] = (base >> 25) & 0xFF
                pkt[7] = (base >> 17) & 0xFF
                pkt[8] = (base >> 9) & 0xFF
                pkt[9] = (base >> 1) & 0xFF
                pkt[10] = ((base & 1) << 7) | 0x7E
                pkt[11] = 0
                body = 12
                room = 176
            remaining = total - pos
            if remaining < room:
                # stuff with an adaptation field so the payload ends the packet
                pad = room - remaining
                if not first:
                    self._header(pid, 0, 3)
                    pkt[4] = pad - 1
                    if pad >= 2:
                        pkt[5] = 0
                        for i in range(6, 4 + pad):
                            pkt[i] = 0xFF
                    body = 4 + pad
                else:
                    # extend the PCR adaptation field
                    pkt[4] = 7 + pad
                    for i in range(12, 12 + pad):
                        pkt[i] = 0xFF
                    body = 12 + pad
                room = remaining
            elif not first:
                self._header(pid, 0, 1)
            # fill room bytes from header then payload
            n = room
            dst = body
            if pos < hlen:
                h = min(hlen - pos, n)
                mv[dst:dst + h] = pes_header[pos:pos + h]
                dst += h
                pos += h
                n -= h
            if n:
                p = pos - hlen
                mv[dst:dst + n] = payload[p:p + n]
                pos += n
            out.push(mv)
            self.packets += 1
            first = False

    def video(self, au, pts, out, pcr=None):
        """One H.264 access unit (Annex-B, AUD included), PTS/PCR in 90 kHz ticks."""
        self._pts(self.pes_video, 9, pts)
        self._pes(PID_VIDEO, self.pes_video, au, pts - 4500 if pcr is None else pcr, out)

    def lpcm(self, pcm_be, pts, out, pcr=None):
        """One LPCM PES: 16-bit big-endian stereo 48 kHz, up to about 1920 bytes."""
        n = len(pcm_be) + 4
        head = bytearray(18)
        head[0:6] = b"\x00\x00\x01\xbd\x00\x00"
        head[4] = ((n + 8) >> 8) & 0xFF
        head[5] = (n + 8) & 0xFF
        head[6] = 0x84
        head[7] = 0x80
        head[8] = 0x05
        self._pts(head, 9, pts)
        head[14] = 0xA0
        head[15] = 6
        head[16] = 0
        head[17] = 0x11
        self._pes(PID_AUDIO, head, pcm_be, pts - 4500 if pcr is None else pcr, out)


class RtpOut:
    """Seven TS packets per RTP packet (payload type 33) over a UDP socket."""

    def __init__(self, sock, addr, ssrc=0x50344341, seq=1):
        self.sock = sock
        self.addr = addr
        self.buf = bytearray(12 + 7 * 188)
        self.mv = memoryview(self.buf)
        self.n = 0
        self.seq = seq
        self.ssrc = ssrc
        self.ts90 = 0
        self.sent = 0
        self.stalls = 0
        self.bytes = 0

    def push(self, pkt):
        off = 12 + self.n * 188
        self.mv[off:off + 188] = pkt
        self.n += 1
        if self.n == 7:
            self.flush()

    def flush(self):
        if not self.n:
            return
        struct.pack_into(">BBHII", self.buf, 0, 0x80, 33, self.seq & 0xFFFF, self.ts90 & 0xFFFFFFFF, self.ssrc)
        end = 12 + self.n * 188
        while True:
            try:
                self.sock.sendto(self.mv[:end], self.addr)
                break
            except OSError:
                self.stalls += 1
                if self.stalls > 100000:
                    raise
                try:
                    import time
                    time.sleep_ms(1)
                except AttributeError:
                    time.sleep(0.001)
        self.seq += 1
        self.sent += 1
        self.bytes += end
        self.n = 0


class FileOut:
    """Collects TS packets into a file (CPython checks)."""

    def __init__(self, f):
        self.f = f
        self.n = 0

    def push(self, pkt):
        self.f.write(pkt)
        self.n += 1

    def flush(self):
        pass
