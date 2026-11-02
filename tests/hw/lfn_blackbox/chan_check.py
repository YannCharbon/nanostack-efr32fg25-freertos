#!/usr/bin/env python3
"""Check channel and timing rules of the LFN against PTI captures (black-box phase 2 analysis).

usage: chan_check.py DIR [DIR...]
Each DIR holds br_pti.txt + br_pti154.pcap and lfn_pti.txt + lfn_pti154.pcap (pti_decode.py
output of the parent's and the LFN's PTI; the same frames in the same order). The LFN's
EUI-64 is LFN_EUI (default kit 440235509), the channel list CHANNELS (default EU plan 32:
0-54, 57-60, 64, 67-68, as wisun-br-linux's regulatory table).

Rules checked, each over every frame that applies:
  1. LFN unicast slot: a parent's unicast frame to the LFN is on DH1CF(slot, LFN EUI-64), the
     slot computed from the LFN's last LUTT-IE (and LUS-IE interval).
  2. LFN broadcast slot: a parent's broadcast LPC/LTS is on DH1CF_bc(LBT slot, LBS BSI).
  3. Discovery: an LPA is on DH1CF(discovery_first_slot + k, LFN EUI-64), k its LND slot.
  4. LTO-IE: the LFN's next LUTT-IE puts slot n (the slot nearest to the LTO frame) at
     s_n + offset, then the adjusted (or EAPOL) interval.
The PTI channel field holds 0x40 | (channel & 0x3f); channels 0-4 and 64-68 are told apart
by the channel list (plan 32 allows 64, 67, 68 but not 65, 66).
"""
import math
import os
import re
import subprocess
import sys

TSHARK = os.environ.get("TSHARK", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../../.deps/tshark/tshark.sh"))
LFN_EUI = os.environ.get("LFN_EUI", "b4:35:22:ff:fe:98:2e:a6")
CHL = list(range(0, 55)) + list(range(57, 61)) + [64, 67, 68]
FIELDS = ["wpan.src64", "wpan.dst64", "wisun.luttie.usn", "wisun.luttie.uio", "wisun.lusie.listen",
          "wisun.lbtie.slot", "wisun.lbtie.bio", "wisun.ltoie.offset", "wisun.ltoie.listening_interval",
          "wisun.lndie.response_delay", "wisun.lndie.discovery_first_slot", "wisun.lbsie.broadcast_id"]
M = 0xffffffff


def rot(x, k):
    return ((x << k) | (x >> (32 - k))) & M


def hashword(k):
    a = b = c = (0xdeadbeef + (3 << 2)) & M
    a, b, c = (a + k[0]) & M, (b + k[1]) & M, (c + k[2]) & M
    c ^= b; c = (c - rot(b, 14)) & M; a ^= c; a = (a - rot(c, 11)) & M; b ^= a; b = (b - rot(a, 25)) & M
    c ^= b; c = (c - rot(b, 16)) & M; a ^= c; a = (a - rot(c, 4)) & M; b ^= a; b = (b - rot(a, 14)) & M
    c ^= b; c = (c - rot(b, 24)) & M
    return c


def dh1cf_uc(slot, eui):
    m = bytes.fromhex(eui.replace(":", ""))
    return CHL[hashword([slot & 0xffff, int.from_bytes(m[4:], "big"), int.from_bytes(m[:4], "big")]) % len(CHL)]


def dh1cf_bc(slot, bsi):
    return CHL[hashword([slot & 0xffff, (bsi << 16) & M, 0]) % len(CHL)]


def raw(ch):
    return 0x40 | (ch & 0x3f)


def load(d, who):
    txt = [l for l in open(os.path.join(d, f"{who}_pti.txt"))]
    out = subprocess.run([TSHARK, "-r", os.path.join(d, f"{who}_pti154.pcap"), "-T", "fields", "-E", "separator=|"] +
                         sum([["-e", f] for f in FIELDS], []), capture_output=True, text=True).stdout.splitlines()
    assert len(out) == len(txt), (d, who, len(out), len(txt))
    rows = []
    for l, t in zip(out, txt):
        v = dict(zip(FIELDS, l.split("|")))
        v["ch"] = int(re.search(r"ch=\s*(\d+)", t).group(1))
        v["t"] = float(t.split()[0])
        v["ft"] = re.search(r"->\S*\s+(\S+)", t).group(1)
        v["who"] = who
        rows.append(v)
    return rows


def check(d):
    ev = sorted(load(d, "br") + load(d, "lfn"), key=lambda x: x["t"])
    n = {k: [0, 0] for k in ("uc", "bc", "lpa", "lto")}
    lut = sweep = None
    I = bsi = None
    pend = None
    for x in ev:
        if x["who"] == "lfn" and x["wisun.luttie.usn"]:
            usn, uio = int(x["wisun.luttie.usn"]), int(x["wisun.luttie.uio"])
            if x["wisun.lusie.listen"]:
                I = int(x["wisun.lusie.listen"])
            s0 = x["t"] - uio / 1000
            if pend and I and x["ft"] != "ACK":     # the ACK of the LTO frame comes before the LFN applies it
                t, off, g = pend
                gs0, gusn, gI = g
                f = (t - gs0) / (gI / 1000)
                nn = gusn + round(f)
                pred = gs0 + (nn - gusn) * gI / 1000 + off / 1000 + (usn - nn) * I / 1000
                ok = abs(s0 - pred) < 0.01
                n["lto"][ok] += 1
                if not ok and os.environ.get("VERBOSE"):
                    print(f"  LTO {off} at {t:.3f}, frac {f - math.floor(f):.3f}: off by {(s0 - pred) * 1000:.0f} ms")
                pend = None
            lut = (s0, usn, I)
            if x["ft"] == "LPAS":
                fs = int(x["wisun.lndie.discovery_first_slot"])
                if not sweep or sweep["fs"] != fs:
                    sweep = {"fs": fs, "ws": x["t"] + int(x["wisun.lndie.response_delay"]) / 1000}
        if x["who"] != "br":
            continue
        if x["wisun.lbsie.broadcast_id"]:
            bsi = int(x["wisun.lbsie.broadcast_id"])
        if x["ft"] in ("LPC", "LTS") and not x["wpan.dst64"] and bsi is not None:
            n["bc"][raw(dh1cf_bc(int(x["wisun.lbtie.slot"]), bsi)) == x["ch"]] += 1
        elif x["ft"] == "LPA" and sweep:
            k = int((x["t"] - sweep["ws"]) / 0.060 + 0.1)   # host times: a few ms of skew between the kits
            n["lpa"][any(raw(dh1cf_uc(sweep["fs"] + k + dk, LFN_EUI)) == x["ch"] for dk in (0, 1))] += 1
        elif x["wpan.dst64"] == LFN_EUI and x["ft"] != "ACK" and lut and lut[2]:
            s0, usn, li = lut
            k = int(((x["t"] - s0) * 1000) / li + 0.01)
            n["uc"][raw(dh1cf_uc(usn + k, LFN_EUI)) == x["ch"]] += 1
        if x["wpan.dst64"] == LFN_EUI and x["wisun.ltoie.offset"] and x["ft"] != "ACK" and lut and lut[2]:
            pend = (x["t"], int(x["wisun.ltoie.offset"]), lut)
    return n


def main():
    tot = {}
    for d in sys.argv[1:]:
        n = check(d)
        print(d, {k: f"{v[1]}/{sum(v)}" for k, v in n.items()})
        for k, v in n.items():
            tot.setdefault(k, [0, 0])
            tot[k][0] += v[0]
            tot[k][1] += v[1]
    print("total (match/frames):", {k: f"{v[1]}/{sum(v)}" for k, v in tot.items()})


if __name__ == "__main__":
    main()
