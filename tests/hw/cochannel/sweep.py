#!/usr/bin/env python3
"""Collision sweep: two frames of equal length, I first, W `delta` us later,
for a list of TX power pairs. Writes <out>/<tag>.jsonl (one line per cycle).

Per block of 10 cycles: 1 with W alone, 1 with I alone (RSSI of each at R and
the no-interference baseline), 8 with both. Every power point runs `--n`
two-frame cycles; a point whose outcome is undecided (either frame received
in 3-97 % of the cycles) is extended to `--n-near` cycles.

usage: sweep.py <out dir> <tag> --delta US [--len 50] [--points A|B|AB|list]
  points A : I at -15 dBm, W from -15 to +15 dBm (SIR of W from about -1 to +22 dB)
  points B : W at -15 dBm, I from -11 to +15 dBm (SIR of W from about -1 to -22 dB)
  --pairs "pW:pI,pW:pI,..." (deci-dBm) for an explicit list
"""
import argparse
import json
import os
import time

from cochannel import Bench, MARK, classify

D_FIRST = 3000          # I's delay after the trigger (us)


def pairs(spec):
    out = []
    if "A" in spec:
        out += [(p, -150) for p in range(-150, 151, 20)]
    if "B" in spec:
        out += [(-150, p) for p in range(-110, 151, 20)]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("tag")
    ap.add_argument("--delta", type=int, required=True)
    ap.add_argument("--len", type=int, default=50)
    ap.add_argument("--ilen", type=int, default=None)
    ap.add_argument("--points", default="AB")
    ap.add_argument("--pairs", default=None)
    ap.add_argument("--n", type=int, default=80)
    ap.add_argument("--n-near", type=int, default=300)
    ap.add_argument("--channel", type=int, default=0)
    ap.add_argument("--dfirst", type=int, default=D_FIRST, help="I's delay after the trigger (us)")
    ap.add_argument("--bitrate", type=int, default=150000)
    ap.add_argument("--shr", type=int, default=14, help="preamble + SFD bytes")
    ap.add_argument("--offset", type=int, default=443, help="sync end at R after trigger end, minus d (us)")
    a = ap.parse_args()
    pl = [tuple(int(x) for x in p.split(":")) for p in a.pairs.split(",")] if a.pairs else pairs(a.points)
    b = Bench(a.out, bitrate=a.bitrate, shr_bytes=a.shr, channel=a.channel)
    b.setup()
    b.set_frame("W", a.len, MARK["W"])
    b.set_frame("I", a.ilen or a.len, MARK["I"])
    dI, dW = a.dfirst, a.dfirst + a.delta
    if dW < a.dfirst - 1500:
        raise SystemExit("delta too negative")
    fn = os.path.join(a.out, f"{a.tag}.jsonl")
    t0 = time.time()
    with open(fn, "a") as f:
        for pW, pI in pl:
            gW = b.set_power("W", pW)
            gI = b.set_power("I", pI)
            recs, n_both, target = [], 0, a.n
            while n_both < target:
                block = []
                for i in range(10):
                    sched = (dW, None) if i == 0 else (None, dI) if i == 1 else (dW, dI)
                    block.append(b.cycle(*sched))
                b.assign(block)
                for r in block:
                    r.update(tag=a.tag, delta=a.delta, wlen=a.len, ilen=a.ilen or a.len, pW=gW, pI=gI,
                             ch=a.channel, bitrate=a.bitrate, res=classify(r, offset_us=a.offset))
                    f.write(json.dumps(r) + "\n")
                recs += block
                both = [r for r in recs if r["valid"] and r["dW"] is not None and r["dI"] is not None]
                n_both = len(both)
                if n_both >= a.n and target == a.n:
                    okW = sum(r["res"]["W"] == "ok" for r in both) / n_both
                    okI = sum(r["res"]["I"] == "ok" for r in both) / n_both
                    if 0.03 < okW < 0.97 or 0.03 < okI < 0.97:
                        target = a.n_near
            f.flush()
            both = [r for r in recs if r["valid"] and r["dW"] is not None and r["dI"] is not None]
            solo = lambda k: [x["rssi"] for r in recs if r["valid"] and r["d" + k] is not None
                              and r["d" + ("I" if k == "W" else "W")] is None
                              for x in r["rx"] if x["crc"] == "Pass" and x["p"][2:3] == [MARK[k]]]
            sw, si = solo("W"), solo("I")
            med = lambda v: sorted(v)[len(v) // 2] if v else None
            okW = sum(r["res"]["W"] == "ok" for r in both)
            okI = sum(r["res"]["I"] == "ok" for r in both)
            inval = sum(not r["valid"] for r in recs)
            print(f"[{time.time() - t0:6.0f}s] pW={gW:5d} pI={gI:5d} rssiW={med(sw)} rssiI={med(si)} "
                  f"n={len(both)} W_ok={okW} I_ok={okI} invalid={inval}", flush=True)


if __name__ == "__main__":
    main()
