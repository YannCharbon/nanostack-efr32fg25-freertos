#!/usr/bin/env python3
"""Decode a raw silink DATA_CHANNEL capture (DCH v2 frames carrying EFR32 PTI).

usage: pti_decode.py RAW.bin [--pcap OUT.pcap] [--host-t0 UNIX_S]

- Writes a pcap with link type 298 (LINKTYPE_SILABS_DEBUG_CHANNEL): one record
  per DCH frame, '[' to ']' included, for Wireshark's silabs-dch dissector.
- Prints one line per PTI frame: DCH time (s, adapter clock), channel, PSDU
  length, frame control, source and destination (last 2 bytes of the EUI-64),
  the Wi-SUN frame type (UTT-IE or LUTT-IE) and the header sub-IEs present.
  Header IEs are not encrypted, so no keys are needed.

PTI payload layout seen on BRD4001A + BRD4271A (adapter FW 1v6p0, RAIL 2.x,
SiSDK 2026.6.2): [DCH seq][hw start 0xFC][PHR 2 B, bit-reversed][PSDU incl.
FCS][appended info 6 B: hw end, ?, ?, channel, ?, protocol].
"""
import argparse
import struct
import sys

FT = {0: "PA", 1: "PAS", 2: "PC", 3: "PCS", 4: "DATA", 5: "ACK", 6: "EAPOL", 9: "LPA", 10: "LPAS",
      11: "LPC", 12: "LPCS", 13: "LTS", 15: "EXT"}
WH = {0x01: "UTT", 0x02: "BT", 0x03: "FC", 0x04: "RSL", 0x05: "MHDS", 0x06: "VH", 0x09: "EA",
      0x0a: "LUTT", 0x0b: "LBT", 0x0c: "NR", 0x0d: "LUS", 0x0e: "FLUS", 0x0f: "LBS", 0x10: "LND",
      0x11: "LTO", 0x12: "PANID", 0xc0: "LBC"}


def rev8(b):
    return int(f"{b:08b}"[::-1], 2)


def frames(data):
    i = 0
    while i + 3 <= len(data):
        if data[i] != 0x5B:
            j = data.find(b"[", i + 1)
            if j < 0:
                return
            i = j
            continue
        L = data[i + 1] | data[i + 2] << 8
        if i + 1 + L >= len(data):
            return
        if data[i + 1 + L] != 0x5D:
            i += 1
            continue
        yield i + 2 + L, data[i:i + 2 + L]
        i += L + 2


def parse_psdu(p):
    """Return dict with fcf fields, src, dst, frame type and WH sub-IE names."""
    r = {"src": "", "dst": "", "ft": "?", "ies": []}
    if len(p) < 3:
        return r
    fcf = p[0] | p[1] << 8
    ftype = fcf & 7
    sec = (fcf >> 3) & 1
    comp = (fcf >> 6) & 1
    seqsup = (fcf >> 8) & 1
    iep = (fcf >> 9) & 1
    dam = (fcf >> 10) & 3
    ver = (fcf >> 12) & 3
    sam = (fcf >> 14) & 3
    r.update(fcf=f"{fcf:04x}", mac_type=ftype, sec=sec)
    k = 2 + (0 if seqsup else 1)
    alen = {0: 0, 2: 2, 3: 8}
    dpan = spani = False
    if ver == 2:
        if dam == 0 and sam == 0:
            dpan = bool(comp)
        elif dam and not sam:
            dpan = not comp
        elif sam and not dam:
            spani = not comp
        elif dam == 3 and sam == 3:
            dpan = not comp
        else:
            dpan = True
            spani = not comp
    else:
        dpan = dam != 0
        spani = sam != 0 and not comp
    if dpan:
        k += 2
    d = p[k:k + alen.get(dam, 0)][::-1]
    k += alen.get(dam, 0)
    if spani:
        k += 2
    s = p[k:k + alen.get(sam, 0)][::-1]
    k += alen.get(sam, 0)
    r["dst"] = d.hex()[-4:] if d else "-"
    r["src"] = s.hex()[-4:] if s else "-"
    if sec and k < len(p):
        sc = p[k]
        k += 1
        if not (sc >> 5) & 1:
            k += 4
        k += {0: 0, 1: 1, 2: 5, 3: 9}[(sc >> 3) & 3]
    if iep:
        while k + 2 <= len(p):
            desc = p[k] | p[k + 1] << 8
            ln = desc & 0x7F
            eid = (desc >> 7) & 0xFF
            body = p[k + 2:k + 2 + ln]
            k += 2 + ln
            if eid == 0x2A and body:
                sub = body[0]
                r["ies"].append(WH.get(sub, f"{sub:02x}"))
                r.setdefault("raw", []).append(f"{WH.get(sub, hex(sub))}={body[1:].hex()}")
                if sub in (0x01, 0x0a) and len(body) > 1:
                    r["ft"] = FT.get(body[1] & 0x0F, str(body[1] & 0x0F))
            if eid in (0x7E, 0x7F):
                break
    if r["ft"] == "?" and ftype == 2:
        r["ft"] = "ACK(imm)"
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("--pcap")
    ap.add_argument("--ie", action="store_true", help="also print the sub-IE bodies (hex)")
    a = ap.parse_args()
    data = open(a.raw, "rb").read()
    out = open(a.pcap, "wb") if a.pcap else None
    if out:
        out.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 298))
    host_off = None
    try:
        import bisect
        idx = [tuple(map(float, l.split())) for l in open(a.raw + ".idx")]
        ends = [int(b) for _, b in idx]
        best = None
        for end, f in frames(data):
            b = f[3:-1]
            if len(b) < 10:
                continue
            k = bisect.bisect_left(ends, end)
            if k < len(idx):
                d = idx[k][0] - int.from_bytes(b[2:8], "little") / 1e6
                best = d if best is None else min(best, d)
        host_off = best
    except FileNotFoundError:
        pass
    for _, f in frames(data):
        body = f[3:-1]
        if len(body) < 10:
            continue
        ts = int.from_bytes(body[2:8], "little")
        typ = body[8] | body[9] << 8
        if out:
            out.write(struct.pack("<IIII", ts // 1000000, ts % 1000000, len(f), len(f)))
            out.write(f)
        if typ != 0x29:
            continue
        p = body[10:]
        if len(p) < 10:
            continue
        start = p[1]
        tail = p[-6:]
        phr_len = rev8(p[3]) | ((rev8(p[2]) & 0x07) << 8)
        psdu = p[4:-6]
        r = parse_psdu(psdu)
        hs = f"{ts / 1e6 + host_off:.3f} " if host_off is not None else ""
        print(f"{hs}{ts / 1e6:14.6f} st={start:02x} end={tail[0]:02x} ch={tail[3]:3d} len={len(psdu):4d} phr={phr_len:4d} "
              f"fcf={r.get('fcf', '')} {r['src']:>4}->{r['dst']:<4} {r['ft']:6s} {','.join(r['ies'])}"
              + ("  " + " ".join(r.get("raw", [])) if a.ie else ""))
    if out:
        out.close()


if __name__ == "__main__":
    main()
