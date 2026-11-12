#!/usr/bin/env python3
"""Tables from sniff.py's output.
usage: analyze.py <out dir> <exp> [--start US]   (start: W's first preamble bit at R,
       µs after the trigger's end; default from the sync events of the run)
Per point (X, and the interferer's power): valid cycles, cycles with a
preamble detection at or after X, the first detection's delay after X
(min / median / max), cycles where W's sync word was detected, and the
preamble left at X (W's preamble end minus X)."""
import json
import statistics as st
import sys
from collections import defaultdict

out, exp = sys.argv[1], sys.argv[2]
recs = [json.loads(l) for l in open(f"{out}/{exp}.jsonl")]
status = {(r["X"], r["pI"]): r["status"] for r in recs if r.get("kind") == "status"}
cyc = [r for r in recs if r.get("kind") != "status" and r.get("valid")]
inval = sum(1 for r in recs if r.get("kind") != "status" and not r.get("valid"))
lp = cyc[0]["lp"]
SHR_US = (lp + 16) / 0.15
# W's sync end at R: the median of the sync events of W-alone cycles where R
# was on before W's frame started, plus the 150 µs between RAIL's
# SYNC1_DETECT event and the frame's sync-end timestamp (calibrated with
# rxPacket timestamps, see the doc); start = sync end - SHR
syncs = [t for r in cyc if r["kind"] == "W" for k, t in r["ev"] if k == "sync"]
sync_ev = st.median(syncs) if syncs else None
start = float(sys.argv[sys.argv.index("--start") + 1]) if "--start" in sys.argv else (sync_ev + 150 - SHR_US)
pre_end = start + lp / 0.15
print(f"{exp}: {len(cyc)} valid cycles ({inval} invalid), preamble {lp} bits, W's preamble at R "
      f"{start:.0f}-{pre_end:.0f} us after the trigger's end (sync events median {sync_ev})")
pts = defaultdict(list)
for r in cyc:
    pts[(r["pI"], r["kind"], r["X"])].append(r)
print("pI kind X left n pd_after_X delay(min/med/max) sync rx_status")
for (pI, kind, X), rs in sorted(pts.items(), key=lambda kv: (kv[0][0] or -999, kv[0][1], kv[0][2])):
    d = []
    ns = 0
    for r in rs:
        pds = [t for k, t in r["ev"] if k == "pd" and t >= X]
        if pds:
            d.append(min(pds) - X)
        if any(k == "sync" for k, t in r["ev"]):
            ns += 1
    dd = f"{min(d)}/{st.median(d):.0f}/{max(d)}" if d else "-"
    s = status.get((X, pI), {})
    print(f"{pI} {kind} {X} {pre_end - X:.0f} {len(rs)} {len(d)} {dd} {ns} {s.get('SyncDetect')}")
