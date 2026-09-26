import sys, struct
def dump(path, max_pes=6):
    data = open(path, "rb").read()
    n = len(data) // 188
    print("==", path, n, "packets")
    pes_seen = 0
    cc = {}
    pcr_last = None
    for i in range(n):
        p = data[i*188:(i+1)*188]
        if p[0] != 0x47:
            print("sync loss at", i); break
        pusi = (p[1] >> 6) & 1
        pid = ((p[1] & 0x1F) << 8) | p[2]
        afc = (p[3] >> 4) & 3
        c = p[3] & 15
        off = 4
        af = ""
        pcr = None
        if afc & 2:
            afl = p[4]
            if afl:
                flags = p[5]
                af = "AF(len=%d flags=%02x%s%s)" % (afl, flags, " RAI" if flags & 0x40 else "", " PCR" if flags & 0x10 else "")
                if flags & 0x10:
                    b = p[6:12]
                    base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
                    pcr = base
                    pcr_last = base
            else:
                af = "AF(len=0)"
            off = 5 + afl
        if i < 6 or (pusi and pid not in (0, 0x1000) and pes_seen < max_pes):
            desc = "pkt %d pid %04x pusi %d afc %d cc %d %s" % (i, pid, pusi, afc, c, af)
            if pid == 0 and pusi:
                sec = p[off+1:]
                desc += " PAT sec_len=%d tsid=%d prog=%d pmt=%04x" % (((sec[1]&0xF)<<8)|sec[2], (sec[3]<<8)|sec[4], (sec[8]<<8)|sec[9], ((sec[10]&0x1F)<<8)|sec[11])
            elif pid == 0x1000 and pusi:
                sec = p[off+1:]
                sl = ((sec[1]&0xF)<<8)|sec[2]
                pcrpid = ((sec[8]&0x1F)<<8)|sec[9]
                pil = ((sec[10]&0xF)<<8)|sec[11]
                streams = sec[12+pil:3+sl-4]
                desc += " PMT sec_len=%d pcr_pid=%04x prog_info=%d streams=%s" % (sl, pcrpid, pil, streams.hex())
            elif pusi and pid not in (0, 0x1000):
                pes = p[off:]
                if pes[:3] == b"\x00\x00\x01":
                    sid = pes[3]; plen = (pes[4] << 8) | pes[5]; f1 = pes[6]; f2 = pes[7]; hl = pes[8]
                    pts = dts = None
                    if f2 & 0x80:
                        b = pes[9:14]
                        pts = ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | (b[4] >> 1)
                    if f2 & 0x40:
                        b = pes[14:19]
                        dts = ((b[0] >> 1) & 7) << 30 | b[1] << 22 | (b[2] >> 1) << 15 | b[3] << 7 | (b[4] >> 1)
                    payload = pes[9+hl:9+hl+8]
                    desc += " PES sid=%02x len=%d f1=%02x f2=%02x hl=%d pts=%s dts=%s pts-pcr=%s payload=%s" % (
                        sid, plen, f1, f2, hl, pts, dts, (pts - pcr_last) if (pts is not None and pcr_last is not None) else None, payload.hex())
                    pes_seen += 1
            print(desc)
    # PCR cadence and null packets
    pids = {}
    for i in range(n):
        p = data[i*188:(i+1)*188]; pid = ((p[1] & 0x1F) << 8) | p[2]; pids[pid] = pids.get(pid, 0) + 1
    print("pids:", {hex(k): v for k, v in sorted(pids.items())})
for f in sys.argv[1:]:
    dump(f)
