"""A minimal Miracast-over-Infrastructure (MS-MICE) source for proving a Windows sink.

Runs under Windows Python. Connects to the sink on TCP 7250, sends Source Ready,
serves the WFD RTSP session on 7236, then streams a pre-muxed MPEG-TS file over
RTP (payload type 33) to the sink's client port, paced at the mux rate.

usage: python mice_source.py SINK_IP TS_FILE [MUXRATE_BPS] [--lpcm]
"""
import os, socket, struct, sys, threading, time, uuid, random

def _opt(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        return sys.argv[i + 1] if i + 1 < len(sys.argv) else True
    return default
POS = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and not sys.argv[i - 1].startswith("--")]
SINK = POS[0]
TS_PATH = POS[1]
MUXRATE = int(POS[2]) if len(POS) > 2 else 6000000
AUDIO = "LPCM 00000002 00" if "--lpcm" in sys.argv else "AAC 00000001 00"
VIDEO_M4 = _opt("--video", "28 00 01 01 00000020 00000000 00000000 00 0000 0000 11 none none")
NAME = _opt("--name", "PyDevices P4")
BIND = _opt("--bind", None)
SESSION_REQUEST = "--session-request" in sys.argv
SECOPTS = int(_opt("--secopts", "0"))
RTSP_PORT = int(_opt("--rtsp-port", "7236"))
T0 = time.time()
LOG = open(_opt("--log", "mice_source.log"), "w", encoding="utf-8")

def log(*a):
    line = "[%7.3f] " % (time.time() - T0) + " ".join(str(x) for x in a)
    print(line, flush=True)
    LOG.write(line + "\n"); LOG.flush()

def tlv(t, v):
    return struct.pack(">BH", t, len(v)) + v

def mice_msg(cmd, tlvs):
    body = b"".join(tlvs)
    return struct.pack(">HBB", 4 + len(body), 1, cmd) + body

source_id = uuid.uuid5(uuid.NAMESPACE_DNS, "cast.pydevices.org/" + _opt("--id", "P4")).bytes
NAME_BYTES = ("\ufeff" + NAME).encode("utf-16-le") if "--no-bom" not in sys.argv else NAME.encode("utf-16-le")

# --- RTSP plumbing -----------------------------------------------------------
class Rtsp:
    def __init__(self, conn):
        self.conn = conn
        self.buf = b""
        self.cseq = 0
        self.session = None
        self.client_port = None
        self.pending = {}   # our CSeq -> label

    def read_msg(self, timeout=30):
        self.conn.settimeout(timeout)
        while b"\r\n\r\n" not in self.buf:
            d = self.conn.recv(4096)
            if not d:
                return None
            self.buf += d
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode("utf-8", "replace").split("\r\n")
        headers = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        clen = int(headers.get("content-length", "0") or 0)
        while len(rest) < clen:
            d = self.conn.recv(4096)
            if not d:
                return None
            rest += d
        body, self.buf = rest[:clen], rest[clen:]
        return lines[0], headers, body.decode("utf-8", "replace")

    def send_raw(self, text):
        log("RTSP >>>\n" + text.rstrip("\r\n"))
        self.conn.sendall(text.encode())

    def request(self, method, target, label, extra="", body=""):
        self.cseq += 1
        self.pending[str(self.cseq)] = label
        msg = "%s %s RTSP/1.0\r\nCSeq: %d\r\n" % (method, target, self.cseq)
        if extra:
            msg += extra
        if body:
            msg += "Content-Type: text/parameters\r\nContent-Length: %d\r\n" % len(body)
        msg += "\r\n" + body
        self.send_raw(msg)

    def respond(self, cseq, extra="", body="", status="200 OK"):
        msg = "RTSP/1.0 %s\r\nCSeq: %s\r\n" % (status, cseq)
        if extra:
            msg += extra
        if body:
            msg += "Content-Type: text/parameters\r\nContent-Length: %d\r\n" % len(body)
        msg += "\r\n" + body
        self.send_raw(msg)

def parse_params(body):
    out = {}
    for ln in body.split("\r\n"):
        if ":" in ln:
            k, v = ln.split(":", 1)
            out[k.strip()] = v.strip()
    return out

# --- RTP streaming ------------------------------------------------------------
stop_flag = threading.Event()

def stream(dst_ip, dst_port):
    data = open(TS_PATH, "rb").read()
    npk = len(data) // 188
    log("stream: %d TS packets (%.1f MB) to %s:%d at %d bps" % (npk, len(data) / 1e6, dst_ip, dst_port, MUXRATE))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    ssrc = random.getrandbits(32)
    seq = random.getrandbits(16)
    per_pkt = 7
    pkt_bytes = per_pkt * 188
    t_start = time.perf_counter()
    sent_bytes = 0
    i = 0
    n_rtp = 0
    while i < npk and not stop_flag.is_set():
        due = sent_bytes * 8.0 / MUXRATE
        now = time.perf_counter() - t_start
        if now < due:
            time.sleep(min(due - now, 0.005))
            continue
        n = min(per_pkt, npk - i)
        ts90 = int(now * 90000) & 0xFFFFFFFF
        hdr = struct.pack(">BBHII", 0x80, 33, seq & 0xFFFF, ts90, ssrc)
        s.sendto(hdr + data[i * 188:(i + n) * 188], (dst_ip, dst_port))
        seq += 1
        i += n
        n_rtp += 1
        sent_bytes += n * 188
        if n_rtp % 2000 == 0:
            log("stream: %d RTP packets, %.1f s" % (n_rtp, now))
    log("stream: done, %d RTP packets, %.1f s" % (n_rtp, time.perf_counter() - t_start))

# --- main ---------------------------------------------------------------------
def main():
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("0.0.0.0", RTSP_PORT))
    ls.listen(1)
    log("RTSP listening on", RTSP_PORT)

    mc = socket.socket()
    mc.settimeout(10)
    if BIND:
        mc.bind((BIND, 0))
    mc.connect((SINK, 7250))
    mc.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log("MICE connected to %s:7250 from %s" % (SINK, mc.getsockname()))
    if SESSION_REQUEST:
        sreq = mice_msg(4, [tlv(0, NAME_BYTES), tlv(3, source_id), tlv(5, bytes([SECOPTS]))])
        mc.sendall(sreq)
        log("MICE >>> Session Request", sreq.hex())
        time.sleep(1.0)
        ready = mice_msg(1, [tlv(2, struct.pack(">H", RTSP_PORT)), tlv(3, source_id)])
    else:
        ready = mice_msg(1, [tlv(0, NAME_BYTES), tlv(2, struct.pack(">H", RTSP_PORT)), tlv(3, source_id)])
    mc.sendall(ready)
    log("MICE >>> Source Ready", ready.hex())

    def mice_reader():
        mc.settimeout(None)
        try:
            while True:
                d = mc.recv(1024)
                if not d:
                    log("MICE <<< closed by sink"); break
                log("MICE <<<", d.hex())
        except Exception as e:
            log("MICE reader:", e)
    threading.Thread(target=mice_reader, daemon=True).start()

    ls.settimeout(40)
    try:
        conn, addr = ls.accept()
    except socket.timeout:
        log("no RTSP connection from the sink within 40 s"); return 2
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    log("RTSP client connected from", addr)
    r = Rtsp(conn)
    r.request("OPTIONS", "*", "M1", extra="Require: org.wfa.wfd1.0\r\n")
    m3_sent = m4_sent = m5_sent = False
    streamer = None
    last_keepalive = time.time()
    deadline = time.time() + 100
    sink_params = {}
    while time.time() < deadline:
        try:
            m = r.read_msg(timeout=5)
        except socket.timeout:
            if r.session and streamer and time.time() - last_keepalive > 10:
                r.request("GET_PARAMETER", "rtsp://localhost/wfd1.0", "M16", extra="Session: %s\r\n" % r.session)
                last_keepalive = time.time()
            if streamer and not streamer.is_alive():
                log("stream finished; tearing down"); break
            continue
        if m is None:
            log("RTSP connection closed by sink"); break
        first, headers, body = m
        log("RTSP <<<\n" + first + "\n" + "\n".join("%s: %s" % kv for kv in headers.items()) + ("\n" + body if body else ""))
        if first.startswith("RTSP/1.0"):
            label = r.pending.pop(headers.get("cseq", ""), "?")
            ok = " 200 " in first
            log("response to %s: %s" % (label, first))
            if label == "M1" and ok:
                r.request("GET_PARAMETER", "rtsp://localhost/wfd1.0", "M3",
                          body="wfd_content_protection\r\nwfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\n")
            elif label == "M3" and ok:
                sink_params = parse_params(body)
                ports = sink_params.get("wfd_client_rtp_ports", "RTP/AVP/UDP;unicast 1028 0 mode=play")
                m4 = ("wfd_video_formats: %s\r\n" % VIDEO_M4 +
                      "wfd_audio_codecs: %s\r\n" % AUDIO +
                      "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n" % conn.getsockname()[0] +
                      "wfd_client_rtp_ports: %s\r\n" % ports)
                r.request("SET_PARAMETER", "rtsp://localhost/wfd1.0", "M4", body=m4)
            elif label == "M4" and ok:
                r.request("SET_PARAMETER", "rtsp://localhost/wfd1.0", "M5", body="wfd_trigger_method: SETUP\r\n")
            elif label == "M4" and not ok:
                log("sink rejected M4"); break
            elif label == "M5" and not ok:
                log("sink rejected M5"); break
            continue
        # a request from the sink
        parts = first.split(" ")
        method = parts[0]
        cseq = headers.get("cseq", "0")
        if method == "OPTIONS":
            r.respond(cseq, extra="Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n")
        elif method == "SETUP":
            tr = headers.get("transport", "")
            port = None
            for tok in tr.split(";"):
                if tok.startswith("client_port="):
                    port = int(tok.split("=")[1].split("-")[0])
            if port is None:
                port = int(sink_params.get("wfd_client_rtp_ports", "x x 1028").split()[1])
            r.client_port = port
            r.session = str(random.randint(100000, 999999))
            r.respond(cseq, extra="Session: %s;timeout=30\r\nTransport: RTP/AVP/UDP;unicast;client_port=%d;server_port=15550\r\n" % (r.session, port))
        elif method == "PLAY":
            r.respond(cseq, extra="Session: %s;timeout=30\r\n" % r.session)
            if streamer is None:
                streamer = threading.Thread(target=stream, args=(addr[0], r.client_port), daemon=True)
                streamer.start()
                last_keepalive = time.time()
        elif method == "TEARDOWN":
            r.respond(cseq, extra="Session: %s\r\n" % r.session)
            log("sink tore down"); break
        elif method in ("GET_PARAMETER", "SET_PARAMETER"):
            if "wfd_idr_request" in body:
                log("sink asked for an IDR")
            r.respond(cseq, extra=("Session: %s\r\n" % r.session) if r.session else "")
        else:
            r.respond(cseq, status="405 Method Not Allowed")
    stop_flag.set()
    try:
        stopm = mice_msg(2, [tlv(0, NAME_BYTES), tlv(3, source_id)])
        mc.sendall(stopm); log("MICE >>> Stop Projection", stopm.hex())
        time.sleep(0.5)
    except Exception as e:
        log("stop:", e)
    conn.close(); mc.close(); ls.close()
    log("done")
    return 0

if __name__ == "__main__":
    sys.exit(main())
