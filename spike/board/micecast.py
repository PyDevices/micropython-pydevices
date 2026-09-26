"""Cast from MicroPython to a Windows PC over Miracast-over-Infrastructure (MS-MICE).

Spike code. One poll loop, no threads: MICE on TCP 7250, the Wi-Fi Display
RTSP session served on 7236, then RTP/UDP from the advertised server port
(Windows opens its firewall for that port only). A *streamer* object made by
``make_streamer(dst_ip, dst_port, server_port)`` produces the RTP: it has
``pump(budget_us)``, ``done`` and ``close()``.
"""
import socket, struct, time, select, network

MICE_PORT = 7250


def utf16(s):
    """UTF-16LE with a byte-order mark, as GNOME's source sends it."""
    return b"\xff\xfe" + "".join(c + "\x00" for c in s if ord(c) < 128).encode()


def tlv(t, v):
    return struct.pack(">BH", t, len(v)) + v


def mice_msg(cmd, tlvs):
    body = b"".join(tlvs)
    return struct.pack(">HBB", 4 + len(body), 1, cmd) + body


def parse_params(body):
    out = {}
    for ln in body.split("\r\n"):
        if ":" in ln:
            k, v = ln.split(":", 1)
            out[k.strip()] = v.strip()
    return out


class Rtsp:
    def __init__(self, conn, log):
        self.conn = conn
        self.log = log
        self.buf = b""
        self.cseq = 0
        self.session = None
        self.client_port = None
        self.pending = {}

    def feed(self):
        d = self.conn.recv(2048)
        if not d:
            return False
        self.buf += d
        return True

    def pop_msg(self):
        if b"\r\n\r\n" not in self.buf:
            return None
        head, rest = self.buf.split(b"\r\n\r\n", 1)
        lines = head.decode().split("\r\n")
        headers = {}
        for ln in lines[1:]:
            if ":" in ln:
                k, v = ln.split(":", 1)
                headers[k.strip().lower()] = v.strip()
        clen = int(headers.get("content-length", "0") or 0)
        if len(rest) < clen:
            return None
        self.buf = rest[clen:]
        return lines[0], headers, rest[:clen].decode()

    def send_raw(self, text):
        self.log("RTSP >>> " + text.replace("\r\n", " | "))
        self.conn.write(text.encode())

    def request(self, method, target, label, extra="", body=""):
        self.cseq += 1
        self.pending[str(self.cseq)] = label
        msg = "%s %s RTSP/1.0\r\nCSeq: %d\r\n" % (method, target, self.cseq) + extra
        if body:
            msg += "Content-Type: text/parameters\r\nContent-Length: %d\r\n" % len(body)
        self.send_raw(msg + "\r\n" + body)

    def respond(self, cseq, extra="", body="", status="200 OK"):
        msg = "RTSP/1.0 %s\r\nCSeq: %s\r\n" % (status, cseq) + extra
        if body:
            msg += "Content-Type: text/parameters\r\nContent-Length: %d\r\n" % len(body)
        self.send_raw(msg + "\r\n" + body)


class Session:
    def __init__(self, sink_ip, name="PyDevices P4", source_id=b"PyDevicesP4cast!", rtsp_port=7236,
                 server_port=15550, video_m4="28 00 01 01 00000020 00000000 00000000 00 0000 0000 11 none none",
                 audio_m4="LPCM 00000002 00", log=print):
        self.sink = sink_ip
        self.name = name
        self.source_id = source_id
        self.rtsp_port = rtsp_port
        self.server_port = server_port
        self.video_m4 = video_m4
        self.audio_m4 = audio_m4
        self.log = log
        self.idr_requests = 0
        self.streamer = None

    def run(self, make_streamer, seconds=3600, idle_after_done=3):
        log = self.log
        w = network.WLAN(network.STA_IF)
        my_ip = w.ifconfig()[0]
        log("ip", my_ip, "rssi", w.status("rssi"))
        try:
            w.config(pm=network.WLAN.PM_NONE)
        except Exception as e:
            log("pm", e)
        ls = socket.socket()
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("0.0.0.0", self.rtsp_port))
        ls.listen(1)
        mc = socket.socket()
        mc.settimeout(10)
        mc.connect((self.sink, MICE_PORT))
        mc.setblocking(False)
        ready = mice_msg(1, [tlv(0, utf16(self.name)), tlv(2, struct.pack(">H", self.rtsp_port)), tlv(3, self.source_id)])
        mc.write(ready)
        log("MICE >>> Source Ready to", self.sink)
        poller = select.poll()
        poller.register(mc, select.POLLIN)
        poller.register(ls, select.POLLIN)
        r = conn = None
        streamer = None
        sink_params = {}
        last_keepalive = time.ticks_ms()
        deadline = time.ticks_add(time.ticks_ms(), seconds * 1000)
        done_at = None
        try:
            while time.ticks_diff(deadline, time.ticks_ms()) > 0:
                if streamer and not streamer.done:
                    streamer.pump(4000)
                    timeout = 0
                else:
                    timeout = 200
                for obj, ev in poller.poll(timeout):
                    if obj is mc:
                        try:
                            d = mc.recv(256)
                        except OSError:
                            d = None
                        if not d:
                            log("MICE <<< closed by sink")
                            poller.unregister(mc)
                            if r is None:
                                return "refused"
                        else:
                            log("MICE <<<", d.hex())
                    elif obj is ls:
                        conn, addr = ls.accept()
                        log("RTSP client connected from", addr)
                        conn.setblocking(False)
                        r = Rtsp(conn, log)
                        poller.register(conn, select.POLLIN)
                        r.request("OPTIONS", "*", "M1", extra="Require: org.wfa.wfd1.0\r\n")
                    elif obj is conn:
                        if not r.feed():
                            log("RTSP closed by sink")
                            return "closed"
                        while True:
                            m = r.pop_msg()
                            if m is None:
                                break
                            first, headers, body = m
                            log("RTSP <<< " + first + " | " + " | ".join("%s: %s" % kv for kv in headers.items()) + (" | " + body.replace("\r\n", " / ") if body else ""))
                            if first.startswith("RTSP/1.0"):
                                label = r.pending.pop(headers.get("cseq", ""), "?")
                                ok = " 200 " in first
                                if label == "M1" and ok:
                                    r.request("GET_PARAMETER", "rtsp://localhost/wfd1.0", "M3",
                                              body="wfd_content_protection\r\nwfd_video_formats\r\nwfd_audio_codecs\r\nwfd_client_rtp_ports\r\n")
                                elif label == "M3" and ok:
                                    sink_params = parse_params(body)
                                    ports = sink_params.get("wfd_client_rtp_ports", "RTP/AVP/UDP;unicast 1028 0 mode=play")
                                    m4 = ("wfd_video_formats: %s\r\n" % self.video_m4 + "wfd_audio_codecs: %s\r\n" % self.audio_m4 +
                                          "wfd_presentation_URL: rtsp://%s/wfd1.0/streamid=0 none\r\n" % my_ip +
                                          "wfd_client_rtp_ports: %s\r\n" % ports)
                                    r.request("SET_PARAMETER", "rtsp://localhost/wfd1.0", "M4", body=m4)
                                elif label == "M4" and ok:
                                    r.request("SET_PARAMETER", "rtsp://localhost/wfd1.0", "M5", body="wfd_trigger_method: SETUP\r\n")
                                elif label in ("M4", "M5") and not ok:
                                    log("sink rejected", label)
                                    return "rejected " + label
                                continue
                            method = first.split(" ")[0]
                            cseq = headers.get("cseq", "0")
                            if method == "OPTIONS":
                                r.respond(cseq, extra="Public: org.wfa.wfd1.0, SETUP, TEARDOWN, PLAY, PAUSE, GET_PARAMETER, SET_PARAMETER\r\n")
                            elif method == "SETUP":
                                port = None
                                for tok in headers.get("transport", "").split(";"):
                                    if tok.startswith("client_port="):
                                        port = int(tok.split("=")[1].split("-")[0])
                                if port is None:
                                    port = int(sink_params.get("wfd_client_rtp_ports", "x x 1028").split()[1])
                                r.client_port = port
                                r.session = "424242"
                                r.respond(cseq, extra="Session: %s;timeout=30\r\nTransport: RTP/AVP/UDP;unicast;client_port=%d;server_port=%d\r\n" % (r.session, port, self.server_port))
                            elif method == "PLAY":
                                r.respond(cseq, extra="Session: %s;timeout=30\r\n" % r.session)
                                if streamer is None:
                                    streamer = make_streamer(addr[0], r.client_port, self.server_port)
                                    self.streamer = streamer
                                    last_keepalive = time.ticks_ms()
                            elif method == "TEARDOWN":
                                r.respond(cseq, extra="Session: %s\r\n" % r.session)
                                log("sink tore down")
                                return "teardown"
                            elif method in ("GET_PARAMETER", "SET_PARAMETER"):
                                if "wfd_idr_request" in body:
                                    self.idr_requests += 1
                                    if streamer and hasattr(streamer, "force_idr"):
                                        streamer.force_idr()
                                r.respond(cseq, extra=("Session: %s\r\n" % r.session) if r.session else "")
                            else:
                                r.respond(cseq, status="405 Method Not Allowed")
                if r and r.session and time.ticks_diff(time.ticks_ms(), last_keepalive) > 10000:
                    r.request("GET_PARAMETER", "rtsp://localhost/wfd1.0", "M16", extra="Session: %s\r\n" % r.session)
                    last_keepalive = time.ticks_ms()
                if streamer and streamer.done:
                    if done_at is None:
                        done_at = time.ticks_ms()
                        log("stream finished")
                    elif time.ticks_diff(time.ticks_ms(), done_at) > idle_after_done * 1000:
                        return "finished"
            return "timeout"
        finally:
            try:
                mc.write(mice_msg(2, [tlv(0, utf16(self.name)), tlv(3, self.source_id)]))
                time.sleep_ms(300)
            except Exception as e:
                log("stop:", e)
            if streamer:
                streamer.close()
            for s in (conn, mc, ls):
                try:
                    s.close()
                except Exception:
                    pass
            log("session closed, %d IDR requests" % self.idr_requests)
