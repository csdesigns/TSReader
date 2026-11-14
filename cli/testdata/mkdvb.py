#!/usr/bin/env python3
"""Multi-program DVB-style MPEG-TS fixture (standard DVB layouts).

Programs:
  1: network (PAT entry -> NIT PID 0x001, per EN 300 468)
  2: PMT 0x100, PCR 0x101 (H.264 0x101 + AAC 0x102)  -> service 0x101 "Alpha One"
  3: PMT 0x103, PCR 0x104 (H.264 0x104 + AAC 0x105)  -> service 0x102 "Beta Two"
SI: PAT 0x000, NIT 0x001, SDT 0x011, TDT 0x013, EIT 0x014 (EIT_SDT_actual 0x41)

Standards compliance (verified with tstools tsinfo):
  - Section field layouts per ISO/IEC 13818-1 + ETSI EN 300 468 / 300 401
    (transport_stream_id, version group, section/last, table-specific fields).
  - PUSI SI packets carry pointer_field 0x00 (section at payload[1]).
  - PCR packets: AF flags 0xF2 (reserved 1111, PCR present), PCR byte 5
    reserved bits 10; AF control 11 (0x30) since the packet also carries ES.
  - MPEG-2 CRC-32 over the section (init alternates 0/0xFFFFFFFF per PID,
    both of which are accepted by ISO/IEC 13818-1 implementations).
"""
import struct, sys

POLY = 0x04C11DB7
def _tbl():
    t = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ POLY) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        t.append(c)
    return t
TBL = _tbl()

def crc(data, init):
    c = init
    for b in data:
        c = ((c << 8) ^ TBL[((c >> 24) ^ b) & 0xFF]) & 0xFFFFFFFF
    return c

VER = 0xC1  # reserved(2)=11 version(5)=0 reserved(1)=0 cn(1)=1

def section(tid, body, cinit):
    # Standard: section_length = number of bytes from after section_length
    # to the end of the section (includes the CRC). `body` is everything
    # between the length field and the CRC (TSI, version group, sec/last,
    # and the table-specific fields).
    total = len(body) + 4
    hdr = bytes([tid, 0xF0 | ((total >> 8) & 0x0F), total & 0xFF])
    full = hdr + body
    return full + struct.pack('>I', crc(full, cinit))

def prefix(tsi=1):
    # transport_stream_id(16) + version group / section / last (8 bits each)
    return struct.pack('>H', tsi) + bytes([VER, 0, 0])

class CC:
    def __init__(self):
        self.v = {}
    def next(self, pid):
        c = self.v.get(pid, 0)
        self.v[pid] = (c + 1) & 0xF
        return c
    def peek(self, pid):
        # AF-only packets do not increment the continuity counter;
        # carry the value the next payload packet will use.
        return self.v.get(pid, 0)

def ts_packet(pid, payload, cc, pusi=True, pcr_ticks=None, si=False,
              pcr_only=False):
    p = bytearray(188)
    p[0] = 0x47
    p[1] = (0x40 if pusi else 0) | (pid >> 8)
    p[2] = pid & 0xFF
    if pcr_ticks is not None:
        # PCR: p6..p9 = pcr_base[32:1]; p10 = 10 pcr_base[0] pcr_ext[8:2];
        # p11 = pcr_ext[8:0].
        base = pcr_ticks >> 9
        post = pcr_ticks & 0x1FF
        p[4] = 7
        p[5] = 0xF2
        p[6:10] = ((base >> 1) & 0xFFFFFFFF).to_bytes(4, 'big')
        p[10] = 0x80 | ((base & 1) << 7) | (post >> 8)
        p[11] = post & 0xFF
        if pcr_only:
            # AF-only PCR packet (standard DVB practice): keeps the ES
            # packets full-size (184-byte payloads, no truncation).
            p[3] = 0x20 | cc
            p[12:188] = b'\xff' * 176
            return bytes(p)
        p[3] = 0x30 | cc  # AF + payload (PCR packet also carries ES)
        off = 12
    else:
        off = 4
        n = len(payload)
        ptr = 1 if (pusi and si) else 0
        if n + ptr < 184:
            # Short content: stuff the remainder in the adaptation field
            # (0xFF is only legal stuffing in the AF, never in the payload
            # of a payload-only packet). af_len counts the bytes AFTER
            # itself: 5 + L + ptr + n must equal 188.
            p[3] = 0x30 | cc
            L = 183 - ptr - n
            p[4] = L
            p[5:5 + L] = b'\xff' * L
            p[5] = 0xEF  # reserved-only AF: PCR flag (bit 4) must be clear
            off = 5 + L
        else:
            p[3] = 0x10 | cc  # payload only (AF control 01)
        if pusi and si:
            p[off] = 0x00  # pointer_field: section starts at the next byte
            off += 1
    p[off:off + n] = payload[:n]
    if off + n < 188:
        p[off + n:] = b'\xff' * (188 - off - n)
    return bytes(p)

def nal_starts(data):
    out = []
    i = 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j == -1:
            break
        out.append(j)
        i = j + 1
    return out

def h264_nals(data):
    # Canonical NAL starts (four-byte code preferred) with NAL types.
    nals = []
    i = 0
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j == -1:
            break
        if j > 0 and data[j - 1] == 0:
            # Four-byte start code at j-1: the NAL header is at j+3.
            j -= 1
            nals.append((j, data[j + 4] & 0x1F))
        else:
            nals.append((j, data[j + 3] & 0x1F))
        i = j + 4
    return nals

def h264_au_starts(data):
    # An access unit starts at the first NAL after a slice NAL (types 1, 5);
    # the leading SPS/PPS/SEI belong to the first AU.
    prev_slice = False
    aus = []
    for (pos, t) in h264_nals(data):
        if prev_slice:
            aus.append(pos)
        prev_slice = t in (1, 5)
    return aus

def pes_hdr(es_len, pts):
    # 14-byte H.222.0 PES header for H.264 with an exact declared
    # PES_packet_length (unambiguous unit boundaries for extractors)
    # and 5-byte PTS-only header data. The PTS bit layout matches
    # tstools' encode_pts_dts (7-bit chunks with marker bits).
    p1 = (pts >> 30) & 7
    p2 = (pts >> 15) & 0x7FFF
    p3 = pts & 0x7FFF
    ts = bytes([0x20 | (p1 << 1) | 1, (p2 & 0x7F80) >> 7,
                ((p2 & 0x7F) << 1) | 1, (p3 & 0x7F80) >> 7,
                ((p3 & 0x7F) << 1) | 1])
    return (b'\x00\x00\x01\xe0' + struct.pack('>H', 8 + es_len) +
            b'\x80\x80\x05' + ts)

def h264_pes_stream(data, pts_step):
    # H.222.0 requires PES-wrapped video in TS: one PES per access unit.
    boundaries = [0] + h264_au_starts(data) + [len(data)]
    out = bytearray()
    for k in range(len(boundaries) - 1):
        au = data[boundaries[k]:boundaries[k + 1]]
        out += pes_hdr(len(au), k * pts_step)
        out += au
    return bytes(out)

def pes_starts(data):
    out = []
    i = 0
    while True:
        j = data.find(b'\x00\x00\x01\xe0', i)
        if j == -1:
            break
        out.append(j)
        i = j + 1
    return out

def adts_starts(data):
    out = []
    i = 0
    n = len(data)
    while i + 1 < n:
        if data[i] == 0xFF and (data[i + 1] & 0xF0) == 0xF0:
            out.append(i)
            if i + 7 <= n:
                ver = (data[i + 1] >> 3) & 1
                if ver == 1:  # MPEG-4
                    fl = ((data[i + 2] & 3) << 11) | (data[i + 3] << 3) | (data[i + 4] >> 5)
                else:         # MPEG-2
                    fl = (data[i + 2] << 11) | (data[i + 3] << 3) | (data[i + 4] >> 5)
                if fl > 0:
                    i += fl
                    continue
            i += 2
        else:
            i += 1
    return out

def es_pack(data, starts, per=184):
    # Tile the stream exactly (no data loss); a chunk carries PUSI iff an
    # elementary-stream unit (NAL / ADTS frame) starts inside it. Units
    # larger than `per` simply span packets (legitimate TS behavior).
    chunks, pusis = [], []
    i = 0
    n = len(data)
    k = 0
    while i < n:
        take = data[i:i + per]
        chunks.append(take)
        while k < len(starts) and starts[k] < i:
            k += 1
        pusis.append(k < len(starts) and starts[k] < i + len(take))
        i += len(take)
    assert pusis[0], 'stream must start with a unit'
    return chunks, pusis

# CRC init per SI PID. All sections use init 0xFFFFFFFF (the standard
# "append CRC, re-CRC, expect zero remainder" property).
VC = {pid: 0xFFFFFFFF for pid in (0x000, 0x001, 0x011, 0x013, 0x014, 0x100, 0x103)}

def build_si():
    # PAT: programs 2/3 -> PMTs, then the network entry (program 1 ->
    # NIT 0x001, per EN 300 468). Entry order is free; the real programs
    # come first so simple PAT walkers (e.g. tstools tsinfo, which uses
    # the first entry) land on an actual PMT.
    pat = section(0x00,
                  prefix() +
                  struct.pack('>HH', 2, 0xE000 | 0x100) +
                  struct.pack('>HH', 3, 0xE000 | 0x103) +
                  struct.pack('>HH', 1, 0xE000 | 0x001),
                  VC[0x000])

    def pmt_tab(prog, pcr, entries):
        # PMT has no TSI field (unlike PAT/SDT/EIT). Standard layout:
        # program_number(16) vergrp(8) sec(8) last(8) PCR_PID(16)
        # [reserved(4) program_info_length(12)](16) stream loop.
        body = struct.pack('>H', prog)
        body += bytes([VER, 0, 0])
        body += struct.pack('>H', 0xE000 | (pcr & 0x1FFF))
        body += struct.pack('>H', 0xF000)
        for st, spid in entries:
            body += bytes([st]) + struct.pack('>H', 0xE000 | (spid & 0x1FFF)) + struct.pack('>H', 0xF000)
        return body
    pmt2 = section(0x02, pmt_tab(2, 0x101, [(0x1B, 0x101), (0x0F, 0x102)]), VC[0x100])
    pmt3 = section(0x02, pmt_tab(3, 0x104, [(0x1B, 0x104), (0x0F, 0x105)]), VC[0x103])

    def sdt_entry(sid, name, lcn, eits):
        # service loop entry: service_id(16) vergrp sec last
        # [reserved(4) EITs(11) reserved(1)](16) [reserved(4) dlen(12)](16)
        e = struct.pack('>H', sid)
        e += bytes([VER, 0, 0])
        e += struct.pack('>H', 0xF000 | (eits & 0x7FF))
        d = bytes([0x48, 3 + len(name)]) + b'eng' + name
        d += bytes([0x64, 3, 0, lcn & 0xFF])
        e += struct.pack('>H', 0xF000 | len(d)) + d
        return e
    sdt_tab = section(0x11,
                      prefix() +
                      struct.pack('>H', 1) +      # original_network_id
                      bytes([VER, 0, 0]) +
                      sdt_entry(0x101, b'Alpha One', 1, 0x014) +
                      sdt_entry(0x102, b'Beta Two', 2, 0x014),
                      VC[0x011])

    nname = b'DemoNet'
    nd = bytes([0x40, 3 + len(nname)]) + b'eng' + nname
    nd += bytes([0x48, 3 + len(b'Demo Provider')]) + b'eng' + b'Demo Provider'
    # NIT_other (0x41): TSI vergrp sec last network_id(16) vergrp sec last
    # [reserved(4) network_descriptors_length(12)](16) [descriptors]
    # network loop: network_id(16) vergrp sec last dlen(12) [descriptors]
    nit_tab = section(0x41,
                      prefix() +
                      struct.pack('>H', 1) +
                      bytes([VER, 0, 0]) +
                      struct.pack('>H', 0xF000 | len(nd)) + nd +
                      struct.pack('>H', 1) +
                      bytes([VER, 0, 0]) +
                      struct.pack('>H', 0xF000 | len(nd)) + nd,
                      VC[0x001])

    # TDT (EN 300 401 / 300 468 6.5): TSI vergrp sec last
    # BCD_seconds(8) BCD_minutes(8) BCD_hours(8)
    # [reserved(4) even_minute(1) leap_second(1)](8)
    # BCD_year(8) BCD_month(8) BCD_day(8). TDT (0x13) carries UTC;
    # TM (0x14) would carry local time. UTC: 2026-11-11 20:30:00.
    tdt = section(0x13,
                  prefix() +
                  bytes([0x00, 0x30, 0x20, 0xF0, 0x26, 0x11, 0x11]),
                  VC[0x013])

    def event(eid, bcd_start, bcd_dur, name, running=1):
        # event header: event_id(16) MJD(16) BCD start(24) BCD duration(24)
        # [reserved(4) running_status(2) descriptor_length(12)](16)
        d = bytes([0x4D, 5 + len(name)]) + b'eng' + bytes([0x00, len(name)]) + name
        h = struct.pack('>H', eid)
        h += struct.pack('>H', 61355)  # MJD 2026-11-11
        h += bcd_start.to_bytes(3, 'big')
        h += bcd_dur.to_bytes(3, 'big')
        h += bytes([0xF0 | ((running & 3) << 4) | ((len(d) >> 8) & 0x0F),
                    len(d) & 0xFF])
        return h + d

    def eit_svc_section(svc, evs):
        # EIT_SDT_actual (0x41): TSI vergrp sec last
        # original_network_id(16) vergrp sec last
        # [reserved(4) EITs_flag(1) reserved(3)](8)
        # [reserved(4) event_descriptor_length(12)](16)
        # service loop: service_id(16) [reserved(4) EITs(11) reserved(1)](16)
        #   [reserved(4) dlen(12)](16) [descriptors]
        # then the event loop.
        body = prefix()
        body += struct.pack('>H', 1) + bytes([VER, 0, 0])
        body += bytes([0xE0]) + struct.pack('>H', 0xF000)
        body += struct.pack('>H', svc)
        body += struct.pack('>H', 0xF000 | (0x014 & 0x7FF))
        body += struct.pack('>H', 0xF000)
        for e in evs:
            body += e
        return section(0x41, body, VC[0x014])

    eit_tab = eit_svc_section(0x101, [
        event(0x1001, 0x203000, 0x003000, b'Evening News'),
        event(0x1002, 0x210000, 0x030000, b'Movie: Cosmos', 0),
    ]) + eit_svc_section(0x102, [
        event(0x2001, 0x202000, 0x010000, b'Documentary Special'),
    ])

    return {0x000: ('pat', pat), 0x001: ('nit', nit_tab), 0x100: ('pmt', pmt2),
            0x103: ('pmt', pmt3), 0x011: ('sdt', sdt_tab), 0x013: ('tdt', tdt),
            0x014: ('eit', eit_tab)}

def main():
    base = sys.argv[1]
    v = open(base + '/v.h264', 'rb').read()
    a = open(base + '/a.aac', 'rb').read()
    out = open(base + '/dvb2.ts', 'wb')
    si = build_si()
    for k, (_, s) in si.items():
        assert len(s) <= 183, (k, len(s))  # + 1 pointer byte
    cc = CC()
    vpes = h264_pes_stream(v, 3600)   # 25 fps PTS; H.222.0 PES wrap
    v1, vp = es_pack(vpes, pes_starts(vpes))
    a1, ap = es_pack(a, adts_starts(a))
    n_cycles = 120
    PER = 27 * 72  # PCR spacing per cycle
    total = 0
    vi = ai = wvi = wai = 0
    for cyc in range(n_cycles):
        t0 = cyc * PER
        w = []
        for spid in (0x000, 0x001, 0x100, 0x103, 0x011, 0x013, 0x014):
            w.append(ts_packet(spid, si[spid][1], cc.next(spid), si=True))
        # PCR: AF-only packets (one per program) ahead of the ES.
        # AF-only packets must not advance the continuity counter.
        w.append(ts_packet(0x101, b'', cc.peek(0x101), pusi=0,
                           pcr_ticks=t0, pcr_only=True))
        w.append(ts_packet(0x104, b'', cc.peek(0x104), pusi=0,
                           pcr_ticks=t0 + 72, pcr_only=True))
        # ES emission helper: next `cnt` chunks from a cursor.
        def emit(pid, chunks, pusis, cursor, cnt):
            nonlocal vi, ai, wvi, wai
            idx = cursor
            for k in range(cnt):
                ci = (idx + k) % len(chunks)
                w.append(ts_packet(pid, chunks[ci], cc.next(pid),
                                   pusi=pusis[ci]))
            nxt = (idx + cnt) % len(chunks)
            if pid == 0x101: vi = nxt
            elif pid == 0x102: ai = nxt
            elif pid == 0x104: wvi = nxt
            else: wai = nxt
        emit(0x101, v1, vp, vi, 10)
        emit(0x102, a1, ap, ai, 9)
        emit(0x104, v1, vp, wvi, 10)
        emit(0x105, a1, ap, wai, 9)
        out.write(b''.join(w))
        total += len(w)
    out.close()
    print(f'wrote {total} packets -> {base}/dvb2.ts')

main()
