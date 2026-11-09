#!/usr/bin/env python3
"""RX-to-TX turnaround on mode 3: W answers R's trigger with txAfterRx d
(d counted from the trigger's sync end; the trigger's PHR + payload + FCS
last 747 us after it). For each d: how many TX succeed, and the gap at R
between the trigger's end (R's own TX end) and W's preamble start
(W's sync end at R - 747 us SHR). Also prints RAIL's state timings.
usage: turnaround.py <out dir>"""
import json
import os
import sys

from cochannel import Bench, MARK

out = sys.argv[1]
b = Bench(out)
b.setup()
for n in ("W", "I"):
    print(n, b.k[n].cmd("setTimings -1 -1 -1 -1 -1 -1 -1", 0.5)[-1])
b.set_frame("W", 50, MARK["W"])
b.set_power("W", -150)
res = []
for d in list(range(700, 1001, 25)) + [1200, 1500]:
    recs = [b.cycle(d, None) for _ in range(20)]
    b.assign(recs)
    ok = [r for r in recs if r.get("W_ok")]
    gaps = [((f["t"] - r["trig_end"]) % (1 << 32)) - 747 for r in ok for f in r["rx"] if f["p"][2:3] == [MARK["W"]]]
    row = {"d": d, "tx_ok": len(ok), "cycles": len(recs), "gap_us": sorted(gaps)}
    res.append(row)
    print(d, len(ok), "/", len(recs), "gap", (min(gaps), max(gaps)) if gaps else None, flush=True)
json.dump(res, open(os.path.join(out, "turnaround.json"), "w"))
