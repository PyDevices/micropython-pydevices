// castif's MPEG-TS muxer as it was at micropython-pydevices ccccb28
// (modules/castif/src/mod_castif.c), extracted verbatim by
// tests/extract_castif_ref.py. compare_castif.c builds it beside tsmux_core.c
// and checks both write the same packets. Do not edit: regenerate.

static uint32_t crc32_mpeg(const uint8_t *data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c ^= (uint32_t)data[i] << 24;
        for (int k = 0; k < 8; k++) {
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        }
    }
    return c;
}

static uint32_t build_section(uint8_t *dst, uint8_t table_id, uint16_t ident,
                              const uint8_t *body, uint32_t body_len) {
    uint32_t length = 5 + body_len + 4;   // counts everything after the length field
    uint32_t n = 0;
    dst[n++] = table_id;
    dst[n++] = (0xB0 | ((length >> 8) & 0x0F));
    dst[n++] = length & 0xFF;
    dst[n++] = ident >> 8;
    dst[n++] = ident & 0xFF;
    dst[n++] = 0xC1;
    dst[n++] = 0;
    dst[n++] = 0;
    memcpy(dst + n, body, body_len);
    n += body_len;
    uint32_t crc = crc32_mpeg(dst, n);
    dst[n++] = crc >> 24;
    dst[n++] = (crc >> 16) & 0xFF;
    dst[n++] = (crc >> 8) & 0xFF;
    dst[n++] = crc & 0xFF;
    return n;
}

static void build_tables(castif_obj_t *c) {
    // PAT: program 1 -> PMT PID
    uint8_t pat_body[4];
    pat_body[0] = 0; pat_body[1] = 1;                       // program_number 1
    pat_body[2] = 0xE0 | (PID_PMT >> 8); pat_body[3] = PID_PMT & 0xFF;
    uint32_t pat_sec = build_section(c->pat + 5, 0x00, 1, pat_body, 4);
    c->pat[4] = 0;                                          // pointer field
    // PMT: PCR PID = video, one video stream (0x1B)
    uint8_t pmt_body[9];
    pmt_body[0] = 0xE0 | (PID_VIDEO >> 8); pmt_body[1] = PID_VIDEO & 0xFF;  // PCR PID
    pmt_body[2] = 0xF0; pmt_body[3] = 0x00;                 // program_info_length 0
    pmt_body[4] = 0x1B;                                     // stream_type H.264
    pmt_body[5] = 0xE0 | (PID_VIDEO >> 8); pmt_body[6] = PID_VIDEO & 0xFF;
    pmt_body[7] = 0xF0; pmt_body[8] = 0x00;                 // ES_info_length 0
    uint8_t pmt_a[9 + 5 + 6];
    memcpy(pmt_a, pmt_body, 9);
    uint32_t pmt_n = 9;
    if (c->audio_on) {
        pmt_a[pmt_n++] = 0x83;                             // HDMV LPCM
        pmt_a[pmt_n++] = 0xE0 | (0x101 >> 8); pmt_a[pmt_n++] = 0x101 & 0xFF;
        pmt_a[pmt_n++] = 0xF0; pmt_a[pmt_n++] = 0x04;      // ES_info_length 4
        pmt_a[pmt_n++] = 0x83; pmt_a[pmt_n++] = 0x02; pmt_a[pmt_n++] = 0x46; pmt_a[pmt_n++] = 0x2f;
    }
    c->pmt_len = build_section(c->pmt + 5, 0x02, 1, pmt_a, pmt_n);
    c->pmt[4] = 0;
    // pad both to 188
    for (uint32_t i = 5 + pat_sec; i < 188; i++) c->pat[i] = 0xFF;
    for (uint32_t i = 5 + c->pmt_len; i < 188; i++) c->pmt[i] = 0xFF;
}

static void ts_header(uint8_t *pkt, uint16_t pid, uint8_t *cc, int pusi, int afc) {
    pkt[0] = 0x47;
    pkt[1] = (pusi ? 0x40 : 0) | (pid >> 8);
    pkt[2] = pid & 0xFF;
    pkt[3] = (afc << 4) | (*cc & 15);
    *cc = (*cc + 1) & 15;
}

static void pts_field(uint8_t *buf, int off, uint32_t pts) {
    buf[off] = 0x21 | ((pts >> 29) & 0x0E);
    buf[off + 1] = (pts >> 22) & 0xFF;
    buf[off + 2] = 0x01 | ((pts >> 14) & 0xFE);
    buf[off + 3] = (pts >> 7) & 0xFF;
    buf[off + 4] = 0x01 | ((pts << 1) & 0xFE);
}

static void mux_video(castif_obj_t *c, const uint8_t *au, uint32_t au_len,
                      uint32_t pts, int key) {
    uint8_t *pkt = c->pkt;
    uint8_t *head = c->pes_head;        // 20 bytes: PES header + AUD
    pts_field(head, 9, pts);
    uint32_t hlen = 20;
    uint32_t total = hlen + au_len;
    uint32_t pos = 0;
    uint32_t pcr = pts - PCR_LEAD;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184;
        uint32_t body = 4;
        if (first) {
            ts_header(pkt, PID_VIDEO, &c->cc_video, 1, 3);
            pkt[4] = 7;
            pkt[5] = key ? 0x50 : 0x10;     // random_access_indicator + PCR flag
            pkt[6] = (pcr >> 25) & 0xFF;
            pkt[7] = (pcr >> 17) & 0xFF;
            pkt[8] = (pcr >> 9) & 0xFF;
            pkt[9] = (pcr >> 1) & 0xFF;
            pkt[10] = ((pcr & 1) << 7) | 0x7E;
            pkt[11] = 0;
            body = 12;
            room = 176;
        }
        uint32_t remaining = total - pos;
        if (remaining < room) {
            uint32_t pad = room - remaining;
            if (!first) {
                ts_header(pkt, PID_VIDEO, &c->cc_video, 0, 3);
                pkt[4] = pad - 1;
                if (pad >= 2) {
                    pkt[5] = 0;
                    for (uint32_t i = 6; i < 4 + pad; i++) pkt[i] = 0xFF;
                }
                body = 4 + pad;
            } else {
                // extend the PCR adaptation field
                pkt[4] = 7 + pad;
                for (uint32_t i = 12; i < 12 + pad; i++) pkt[i] = 0xFF;
                body = 12 + pad;
            }
            room = remaining;
        } else if (!first) {
            ts_header(pkt, PID_VIDEO, &c->cc_video, 0, 1);
        }
        uint32_t n = room;
        uint32_t dst = body;
        if (pos < hlen) {
            uint32_t hh = (hlen - pos < n) ? (hlen - pos) : n;
            memcpy(pkt + dst, head + pos, hh);
            dst += hh; pos += hh; n -= hh;
        }
        if (n) {
            memcpy(pkt + dst, au + (pos - hlen), n);
            pos += n;
        }
        rtp_push(c, pkt);
        first = 0;
    }
}

static void mux_lpcm(castif_obj_t *c, const uint8_t *pcm, uint32_t pts) {
    uint8_t *pkt = c->pkt;
    uint8_t *head = c->apes;               // 18 bytes
    uint32_t n = A_BLOCK + 4;
    head[0]=0;head[1]=0;head[2]=1;head[3]=0xbd;
    head[4]=((n+8)>>8)&0xFF; head[5]=(n+8)&0xFF;
    head[6]=0x80; head[7]=0x80; head[8]=0x05;
    pts_field(head, 9, pts);
    head[14]=0xA0; head[15]=6; head[16]=0; head[17]=0x11;
    uint32_t hlen = 18, total = hlen + A_BLOCK, pos = 0;
    int first = 1;
    while (pos < total) {
        uint32_t room = 184, body = 4;
        uint32_t remaining = total - pos;
        if (remaining < room) {
            uint32_t pad = room - remaining;
            ts_header(pkt, 0x101, &c->cc_audio, first, 3);
            pkt[4] = pad - 1;
            if (pad >= 2) { pkt[5] = 0; for (uint32_t i = 6; i < 4 + pad; i++) pkt[i] = 0xFF; }
            body = 4 + pad; room = remaining;
        } else {
            ts_header(pkt, 0x101, &c->cc_audio, first, 1);
        }
        uint32_t k = room, dst = body;
        if (pos < hlen) { uint32_t hh = (hlen - pos < k) ? (hlen - pos) : k; memcpy(pkt+dst, head+pos, hh); dst+=hh; pos+=hh; k-=hh; }
        if (k) {
            // the ring holds the pump's native little-endian PCM; LPCM on the
            // wire is big-endian, so swap as we copy (offsets are even: the
            // 18-byte header and 184-byte payloads keep samples whole)
            uint32_t off = pos - hlen;
            for (uint32_t i = 0; i < k; i++) pkt[dst + i] = pcm[(off + i) ^ 1];
            pos += k;
        }
        rtp_push(c, pkt);
        first = 0;
    }
}

static void emit_tables(castif_obj_t *c) {
    c->pat[3] = (1 << 4) | (c->cc_pat & 15); c->cc_pat = (c->cc_pat + 1) & 15;
    c->pat[0] = 0x47; c->pat[1] = 0x40; c->pat[2] = 0x00;
    rtp_push(c, c->pat);
    c->pmt[3] = (1 << 4) | (c->cc_pmt & 15); c->cc_pmt = (c->cc_pmt + 1) & 15;
    c->pmt[0] = 0x47; c->pmt[1] = 0x40 | (PID_PMT >> 8); c->pmt[2] = PID_PMT & 0xFF;
    rtp_push(c, c->pmt);
}

static void mux_pcr_only(castif_obj_t *c, uint32_t pcr) {
    // a PCR with no payload on the video PID, for ticks whose frame was skipped:
    // the sink's clock keeps running while the picture is static and audio plays
    uint8_t *pkt = c->pkt;
    pkt[0] = 0x47; pkt[1] = PID_VIDEO >> 8; pkt[2] = PID_VIDEO & 0xFF;
    pkt[3] = (2 << 4) | ((c->cc_video - 1) & 15);   // adaptation field only: cc does not advance
    pkt[4] = 183;
    pkt[5] = 0x10;
    pkt[6] = (pcr >> 25) & 0xFF; pkt[7] = (pcr >> 17) & 0xFF;
    pkt[8] = (pcr >> 9) & 0xFF; pkt[9] = (pcr >> 1) & 0xFF;
    pkt[10] = ((pcr & 1) << 7) | 0x7E; pkt[11] = 0;
    memset(pkt + 12, 0xFF, 176);
    rtp_push(c, pkt);
}
