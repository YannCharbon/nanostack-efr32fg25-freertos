#!/usr/bin/env python3
"""S1 cold joins with wsbrd + wisun_rcp (kit B) as the parent: restart wsbrd (fresh storage,
so a new PAN), power-cycle the LFN (kit A), clear its credential cache, join, wait for
[Connected]. Kit B's PTI should run (pti.py start 440240046 DIR/br_dch.bin); the LFN's PTI is
handled here (DIR/lfn_dch.bin). The LFN console must run (wsbrd.py start DIR).
Results: DIR/s1_reps.json (same format as s1_run.py, so s1_stats.py works unchanged).

usage: s1w_run.py DIR [--reps 10] [--post-s 30] [--lfn-trace all,info] [--extra FILE]
"""
import argparse
import json
import os
import time

import pti
import wsbrd
from s1_run import LFN, reset, send, size, wait


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--rep0", type=int, default=0)
    ap.add_argument("--post-s", type=int, default=30)
    ap.add_argument("--lfn-trace", default="all,info")
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--extra")
    a = ap.parse_args()
    d = os.path.abspath(a.dir)
    jf = os.path.join(d, "s1_reps.json")
    res = json.load(open(jf)) if os.path.exists(jf) else []
    res = [r for r in res if r["rep"] < a.rep0]
    lfn_pti = os.path.join(d, "lfn_dch.bin")
    for rep in range(a.rep0, a.rep0 + a.reps):
        r = {"rep": rep}
        pti.stop(LFN, lfn_pti)
        wsbrd.stop_wsbrd(d)
        r["t_br_reset"] = time.time()
        pcap, t_ready = wsbrd.start_wsbrd(d, a.extra)
        r["t_br_started"], r["pcap"] = t_ready, os.path.basename(pcap)
        for attempt in range(4):
            p0 = size(d, "lfn")
            r["t_lfn_reset"] = time.time()
            reset(LFN, 6.0 + 6.0 * attempt)
            if wait(d, "lfn", r"Wi-SUN LFN CLI Application", p0, 10):
                break
            print(f"rep {rep}: LFN reset attempt {attempt} not seen, retrying", flush=True)
        pti.start(LFN, lfn_pti)
        time.sleep(3)
        send(d, "lfn", "wisun clear_credential_cache")
        time.sleep(1.5)
        send(d, "lfn", f"wisun set_trace_level {a.lfn_trace}")
        time.sleep(1.5)
        pos = size(d, "lfn")
        r["log_offset_lfn"] = pos
        r["t_join"] = time.time()
        send(d, "lfn", "wisun join_fan11")
        m = wait(d, "lfn", r"\[Connected: (\d+) s\]|\[Failed[^\]]*\]", pos, a.timeout)
        if m and m.group(0).startswith("[Failed"):
            print(f"rep {rep}: {m.group(0)}", flush=True)
            m = None
        r["t_connected"] = time.time() if m else None
        r["connected_stack_s"] = int(m.group(1)) if m else None
        print(f"rep {rep}: connected after {r['t_connected'] - r['t_join']:.1f} s" if m else f"rep {rep}: no join",
              flush=True)
        time.sleep(a.post_s)
        res.append(r)
        with open(jf, "w") as f:
            json.dump(res, f, indent=1)


if __name__ == "__main__":
    main()
